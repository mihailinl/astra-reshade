#include "layer.h"
#include "guard.h"
#include "link.h"
#include "log.h"
#include "pretone_rules.h"
#include "ring.h"
#include "tone.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>
#include <reshade.hpp>

using namespace reshade::api;

namespace layer
{
	namespace
	{
		constexpr const char *kEffect = "AstraPassthrough.fx";
		constexpr float kHostFar = 1.0e7f; // a projection with no far plane (Unreal's): reversed Z, infinite

		struct Layer
		{
			resource tex = {0};
			resource_view srv = {0};
		};

		std::atomic<bool> g_active{false}, g_hdrPass{false};
		std::atomic<unsigned long long> g_herRuns{0};
		Layer g_toneLut; // tone.h's curve and its inverse (ASTRATONE), for her pass before the game's tonemapper
		bool g_toneFailed = false;
		float g_toneExposure = 1.0f;
		std::mutex g_lock; // host pose, shadow strength (written by the link on the present thread; the effects may run elsewhere)
		view::Pose g_host;
		float g_hostNear = 0.1f, g_hostFar = 0.0f;
		float g_shadow = 0.0f;

		wchar_t g_name[128] = L"Local\\astra-frame", g_path[260] = L"Z:\\dev\\shm\\astra-frame";
		bool g_named = false; // no hello yet: nothing to map
		HANDLE g_file = INVALID_HANDLE_VALUE, g_mapping = nullptr;
		const uint8_t *g_view = nullptr;
		ring::Header g_header;
		int64_t g_lastPublish = -1;
		ULONGLONG g_nextOpen = 0;

		Layer g_world, g_depth, g_sunMap;
		uint32_t g_texW = 0, g_texH = 0, g_sunN = 0;
		std::vector<uint8_t> g_colourCopy, g_depthCopy, g_sunCopy;
		ring::Frame g_frame;     // the picture shown
		bool g_hasFrame = false; // one was uploaded since the ring was mapped
		std::atomic<unsigned long long> g_frames{0}, g_wantFrame{0}, g_waitTimeouts{0};
		float g_waitMs = 6.0f;
		char g_status[200] = "ring: waiting for the engine's hello";

		void close_mapping()
		{
			if (g_view != nullptr)
				UnmapViewOfFile(g_view);
			if (g_mapping != nullptr)
				CloseHandle(g_mapping);
			if (g_file != INVALID_HANDLE_VALUE)
				CloseHandle(g_file);
			g_view = nullptr;
			g_mapping = nullptr;
			g_file = INVALID_HANDLE_VALUE;
			g_hasFrame = false;
		}

		/// The engine's frames: a Win32 named mapping (Windows), else Wine's view of the /dev/shm file (Linux).
		bool open_mapping()
		{
			if (g_view != nullptr)
				return true;
			if (!g_named || GetTickCount64() < g_nextOpen)
				return false;
			g_nextOpen = GetTickCount64() + 1000;
			g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, g_name);
			if (g_mapping == nullptr)
			{
				g_file = CreateFileW(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
					nullptr);
				if (g_file == INVALID_HANDLE_VALUE)
					return false;
				g_mapping = CreateFileMappingW(g_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
			}
			if (g_mapping == nullptr)
			{
				close_mapping();
				return false;
			}
			const auto *head = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, ring::kHeader));
			ring::Header h;
			const bool ok = head != nullptr && ring::read_header(head, ring::kHeader, h);
			if (head != nullptr)
				UnmapViewOfFile(head);
			if (!ok)
			{
				close_mapping();
				return false;
			}
			// sized from the header, never from a constant (3 slots before views, 6 since)
			g_view = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, h.bytes()));
			if (g_view == nullptr)
			{
				close_mapping();
				return false;
			}
			g_header = h;
			g_lastPublish = -1;
			std::snprintf(g_status, sizeof(g_status), "ring: mapped (%d slots, up to %ux%u)", h.slots, h.max_w, h.max_h);
			logf("layer: connected to the engine's frames (%d slots, up to %ux%u)", h.slots, h.max_w, h.max_h);
			return true;
		}

		/// Copies out of the ring with faults contained: a ring the engine shrank or removed under us must not
		/// take the game down.
		bool guarded_copy(void *dst, const void *src, size_t n)
		{
			return guard::run("reading the engine's frame", [&] { std::memcpy(dst, src, n); });
		}

		void destroy(device *dev, Layer &l)
		{
			if (l.srv.handle != 0)
				dev->destroy_resource_view(l.srv);
			if (l.tex.handle != 0)
				dev->destroy_resource(l.tex);
			l = Layer();
		}

		// A texture replaced (her picture changed size) may still be read by a frame in flight: destroyed kFrames runs
		// of the effects later (one run per frame), never at once.
		uint64_t g_runs = 0;
		pretone_rules::Retired<Layer> g_retiredLayers;

		void retire(Layer &l)
		{
			if (l.tex.handle != 0 || l.srv.handle != 0)
				g_retiredLayers.retire(l, g_runs);
			l = Layer();
		}

		void destroy_staging(device *dev); // her upload buffers (below)

		void destroy_layers(device *dev)
		{
			destroy(dev, g_world);
			destroy(dev, g_depth);
			destroy(dev, g_sunMap);
			destroy(dev, g_toneLut);
			g_retiredLayers.collect(~uint64_t(0) >> 1, [dev](Layer &l) { destroy(dev, l); }); // the GPU was waited for
			g_toneFailed = false;
			destroy_staging(dev);
			g_texW = g_texH = g_sunN = 0;
			g_hasFrame = false;
		}

		bool create_layer(device *dev, Layer &l, uint32_t w, uint32_t h, format fmt)
		{
			if (!dev->create_resource(resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest),
					nullptr, resource_usage::shader_resource, &l.tex))
				return false;
			return dev->create_resource_view(l.tex, resource_usage::shader_resource, resource_view_desc(fmt), &l.srv);
		}

		void set_float3(effect_runtime *runtime, const char *name, float x, float y, float z)
		{
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, name); v.handle != 0)
				runtime->set_uniform_value_float(v, x, y, z);
		}

		void set_float4(effect_runtime *runtime, const char *name, float x, float y, float z, float w)
		{
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, name); v.handle != 0)
				runtime->set_uniform_value_float(v, x, y, z, w);
		}

		void set_bool(effect_runtime *runtime, const char *name, bool b)
		{
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, name); v.handle != 0)
				runtime->set_uniform_value_bool(v, b);
		}

		void bind(effect_runtime *runtime)
		{
			runtime->update_texture_bindings("ASTRAWORLD", g_world.srv, g_world.srv);
			runtime->update_texture_bindings("ASTRADEPTH", g_depth.srv, g_depth.srv);
			runtime->update_texture_bindings("ASTRASUN", g_sunMap.srv, g_sunMap.srv);
			runtime->update_texture_bindings("ASTRATONE", g_toneLut.srv, g_toneLut.srv);
		}

		/// tone.h's model into the effect's uniforms (only her pass before the tonemapper reads them).
		void set_tone(effect_runtime *runtime)
		{
			static tone::Uniform u[tone::kUniforms];
			static float shape[4];
			static bool made = false;
			if (!made)
			{
				tone::uniforms(u);
				tone::lut_shape(shape);
				made = true;
			}
			for (const tone::Uniform &x : u)
				set_float3(runtime, x.name, x.v[0], x.v[1], x.v[2]);
			set_float4(runtime, "ToneLut", shape[0], shape[1], shape[2], shape[3]);
		}

		/// On the present thread: through ReShade's immediate command list (as ReShade uploads its own textures - out of
		/// the shader-resource state and back around the copy).
		void upload_plane(effect_runtime *runtime, Layer &l, const void *data, uint32_t w, uint32_t h)
		{
			command_list *imm = runtime->get_command_queue()->get_immediate_command_list();
			if (imm == nullptr)
				return;
			subresource_data d;
			d.data = const_cast<void *>(data);
			d.row_pitch = w * 4;
			d.slice_pitch = w * h * 4;
			imm->barrier(l.tex, resource_usage::shader_resource, resource_usage::copy_dest);
			imm->update_texture_region(d, l.tex, 0);
			imm->barrier(l.tex, resource_usage::copy_dest, resource_usage::shader_resource);
		}

		/// The tone table (tone.h: the inverse of Unreal's default film curve, and the curve), made once per device.
		bool make_tone_lut(effect_runtime *runtime)
		{
			if (g_toneLut.tex.handle != 0)
				return true;
			if (g_toneFailed)
				return false;
			device *dev = runtime->get_device();
			static std::vector<float> table;
			if (table.empty())
			{
				table.resize(2 * tone::kLutN);
				tone::fill_lut(table.data());
			}
			if (!create_layer(dev, g_toneLut, tone::kLutN, 2, format::r32_float))
			{
				destroy(dev, g_toneLut);
				g_toneFailed = true;
				logf("layer: cannot make the tone table: she is not drawn before the game's tonemapper");
				return false;
			}
			upload_plane(runtime, g_toneLut, table.data(), tone::kLutN, 2); // R32F: 4 bytes a texel, as RGBA8
			bind(runtime);
			return true;
		}

		// Her pass before the game's tonemapper runs on a game thread recording a command list. ReShade's
		// update_texture_region goes through its immediate command list, which the game's submitting thread flushes
		// under a lock this thread does not hold - so there her picture is copied by the pass's own command list, from
		// an upload buffer of a small ring, each reused only kReuse frames after the GPU was given it.
		struct Staging
		{
			resource buf = {0};
			uint64_t size = 0;
			uint64_t used_at = 0;
			bool used = false;
		};
		constexpr int kStaging = 5;
		constexpr uint64_t kReuse = 4;
		Staging g_staging[kStaging];
		std::atomic<uint64_t> g_passFrame{0}; // the frame her pass before the tonemapper draws (pretone's count)

		struct Plane
		{
			Layer *layer;
			const std::vector<uint8_t> *data;
			uint32_t w, h;
		};

		uint64_t pitch_of(uint32_t w)
		{
			return (uint64_t(w) * 4 + 255) & ~uint64_t(255); // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
		}

		/// The planes into their textures through `cmd`. False: no upload buffer free (the GPU still reads them all) or
		/// none could be made - the textures keep the last picture.
		bool upload_through(device *dev, command_list *cmd, const Plane *planes, int n)
		{
			const uint64_t frame = g_passFrame;
			uint64_t offsets[3] = {}, total = 0;
			for (int i = 0; i < n; ++i)
			{
				offsets[i] = total;
				total += (pitch_of(planes[i].w) * planes[i].h + 511) & ~uint64_t(511); // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
			}
			Staging *slot = nullptr;
			for (Staging &st : g_staging)
				if (!st.used || frame >= st.used_at + kReuse)
				{
					slot = &st;
					break;
				}
			if (slot == nullptr)
				return false;
			if (slot->size < total)
			{
				if (slot->buf.handle != 0)
					dev->destroy_resource(slot->buf); // not in flight: its last use is kReuse frames back
				slot->buf = {0};
				slot->size = 0;
				if (!dev->create_resource(resource_desc(total, memory_heap::upload, resource_usage::copy_source), nullptr, resource_usage::cpu_access,
						&slot->buf))
				{
					slot->buf = {0};
					return false;
				}
				slot->size = total;
			}
			void *mapped = nullptr;
			if (!dev->map_buffer_region(slot->buf, 0, UINT64_MAX, map_access::write_only, &mapped) || mapped == nullptr)
				return false;
			for (int i = 0; i < n; ++i)
			{
				const Plane &p = planes[i];
				const uint64_t pitch = pitch_of(p.w);
				for (uint32_t y = 0; y < p.h; ++y)
					std::memcpy(static_cast<uint8_t *>(mapped) + offsets[i] + y * pitch, p.data->data() + size_t(y) * p.w * 4, size_t(p.w) * 4);
			}
			dev->unmap_buffer_region(slot->buf);
			for (int i = 0; i < n; ++i)
			{
				cmd->barrier(planes[i].layer->tex, resource_usage::shader_resource, resource_usage::copy_dest);
				cmd->copy_buffer_to_texture(slot->buf, offsets[i], 0, 0, planes[i].layer->tex, 0, nullptr);
				cmd->barrier(planes[i].layer->tex, resource_usage::copy_dest, resource_usage::shader_resource);
			}
			slot->used = true;
			slot->used_at = frame;
			return true;
		}

		void destroy_staging(device *dev)
		{
			for (Staging &st : g_staging)
			{
				if (st.buf.handle != 0)
					dev->destroy_resource(st.buf);
				st = Staging();
			}
		}

		/// The newest published frame, if not shown yet: on the present thread through ReShade's upload, before the
		/// game's tonemapper through `cmd` (upload_through).
		void upload(effect_runtime *runtime, command_list *cmd)
		{
			if (*reinterpret_cast<const volatile uint32_t *>(g_view) != ring::kMagic)
			{
				close_mapping(); // the engine made a new one
				g_nextOpen = 0;
				return;
			}
			ring::Frame f;
			const ring::Result r = ring::read_latest(g_view, g_header, g_lastPublish, guarded_copy, f, g_colourCopy, g_depthCopy, g_sunCopy);
			if (r == ring::Result::fault)
			{
				logf("layer: the engine's ring faulted while read: mapping it again");
				close_mapping();
				return;
			}
			if (r != ring::Result::ok)
				return;
			device *dev = runtime->get_device();
			if (!f.empty)
			{
				if (f.rw != g_texW || f.rh != g_texH)
				{
					retire(g_world);
					retire(g_depth);
					g_texW = g_texH = 0;
					if (!create_layer(dev, g_world, f.rw, f.rh, format::r8g8b8a8_unorm) || !create_layer(dev, g_depth, f.rw, f.rh, format::r32_float))
					{
						destroy(dev, g_world); // never used by the GPU
						destroy(dev, g_depth);
						return;
					}
					g_texW = f.rw;
					g_texH = f.rh;
					bind(runtime);
				}
				if (f.sun.valid && f.sun.n != g_sunN)
				{
					retire(g_sunMap);
					g_sunN = create_layer(dev, g_sunMap, f.sun.n, f.sun.n, format::r32_float) ? f.sun.n : 0;
					bind(runtime);
				}
				const bool sun = f.sun.valid && g_sunN == f.sun.n;
				if (g_hdrPass)
				{
					Plane planes[3] = {{&g_world, &g_colourCopy, f.rw, f.rh}, {&g_depth, &g_depthCopy, f.rw, f.rh}, {&g_sunMap, &g_sunCopy, f.sun.n, f.sun.n}};
					if (!upload_through(dev, cmd, planes, sun ? 3 : 2))
						return; // read again next time (g_lastPublish not advanced): the last picture is re-projected
				}
				else
				{
					upload_plane(runtime, g_world, g_colourCopy.data(), f.rw, f.rh);
					upload_plane(runtime, g_depth, g_depthCopy.data(), f.rw, f.rh);
					if (sun)
						upload_plane(runtime, g_sunMap, g_sunCopy.data(), f.sun.n, f.sun.n);
				}
			}
			g_lastPublish = f.publish;
			g_frame = f;
			g_hasFrame = true;
			++g_frames;
		}

		/// Her picture for the camera being presented is usually a millisecond or two away: wait for it (bounded)
		/// instead of re-projecting an older one, so she sits exactly on this frame.
		void wait_for_frame()
		{
			const unsigned long long want = g_wantFrame;
			const float budget = g_waitMs;
			if (want == 0 || budget <= 0.0f || g_view == nullptr)
				return;
			LARGE_INTEGER f, t0, t;
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&t0);
			for (;;)
			{
				if (ring::latest_host_frame(g_view, g_header) >= want)
					break;
				QueryPerformanceCounter(&t);
				if (float(t.QuadPart - t0.QuadPart) * 1000.0f / float(f.QuadPart) > budget)
				{
					++g_waitTimeouts;
					break;
				}
				YieldProcessor();
				SwitchToThread();
			}
		}

		/// world vector -> camera space of `p` (the transpose of its camera -> world columns)
		void to_camera(const view::Pose &p, const float v[3], float out[3])
		{
			for (int i = 0; i < 3; ++i)
				out[i] = p.col[i][0] * v[0] + p.col[i][1] * v[1] + p.col[i][2] * v[2];
		}

		void on_begin_effects(effect_runtime *runtime, command_list *cmd, resource_view, resource_view)
		{
			++g_runs;
			{
				device *dev = runtime->get_device();
				g_retiredLayers.collect(g_runs, [dev](Layer &l) { destroy(dev, l); });
			}
			// into the game's HDR scene colour (before its tonemapper), or onto the presented picture
			const bool hdr = g_hdrPass;
			set_bool(runtime, "AstraHdr", hdr);
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "ToneExposure"); v.handle != 0)
				runtime->set_uniform_value_float(v, g_toneExposure);
			if (!hdr)
				make_tone_lut(runtime); // on the present thread only (an upload): pretone waits for it (tone_ready)
			bool on = g_active && (!hdr || g_toneLut.tex.handle != 0) && open_mapping();
			if (hdr && on)
				set_tone(runtime);
			if (on)
			{
				wait_for_frame();
				upload(runtime, cmd);
			}
			// the camera her picture was drawn with (the link remembers the cameras it sent)
			view::Pose from;
			float aspect = 1.0f;
			double origin[3] = {};
			on = on && g_hasFrame && !g_frame.empty && g_texW != 0 && enginelink::pose_of(g_frame.host_frame, from, aspect, origin);
			set_bool(runtime, "AstraActive", on);
			if (!on)
				return;
			view::Pose host;
			float host_near, host_far, shadow;
			{
				std::lock_guard<std::mutex> lock(g_lock);
				host = g_host;
				host_near = g_hostNear;
				host_far = g_hostFar > host_near ? g_hostFar : kHostFar;
				shadow = g_shadow;
			}
			if (!host.valid)
				host = from;
			from.tan_half_v = std::tan(g_frame.vfov_deg * 3.14159265f / 360.0f); // what it was drawn at
			// she moved on since her picture (running, riding): as if its camera had moved along with her
			double moved[3] = {0, 0, 0};
			enginelink::moved_since(g_frame.host_frame, moved);
			const view::Warp w = view::warp(host, from, moved);

			set_float3(runtime, "AstraPlanes", g_frame.near_m, g_frame.far_m, float(g_frame.flags));
			set_float3(runtime, "HostPlanes", host_near, host_far, 0.0f);
			// the planes hold her rectangle of the whole picture (flag 128), or all of it
			set_float4(runtime, "AstraRect", float(g_frame.rx) / float(g_frame.w), float(g_frame.ry) / float(g_frame.h), float(g_frame.rw) / float(g_frame.w),
				float(g_frame.rh) / float(g_frame.h));
			set_bool(runtime, "AstraWarpIdentity", w.identity);
			set_float3(runtime, "AstraWarpRow0", w.row[0][0], w.row[0][1], w.row[0][2]);
			set_float3(runtime, "AstraWarpRow1", w.row[1][0], w.row[1][1], w.row[1][2]);
			set_float3(runtime, "AstraWarpRow2", w.row[2][0], w.row[2][1], w.row[2][2]);
			set_float3(runtime, "AstraWarpT", w.t[0], w.t[1], w.t[2]);
			set_float3(runtime, "AstraWarpTan", host.tan_half_v, from.tan_half_v, aspect);

			// her shadow on the game's ground: her view from the sun, in the presented camera's space
			const bool sun_map = g_frame.sun.valid && g_sunN != 0 && shadow > 0.0f;
			if (sun_map)
			{
				const ring::SunView &s = g_frame.sun;
				// the engine's world in that frame is absolute - origin; moved along with her like her picture
				const float cd[3] = {float(s.center[0] + origin[0] + moved[0] - host.pos[0]), float(s.center[1] + origin[1] + moved[1] - host.pos[1]),
					float(s.center[2] + origin[2] + moved[2] - host.pos[2])};
				float c[3], r[3], u[3], d[3];
				to_camera(host, cd, c);
				to_camera(host, s.right, r);
				to_camera(host, s.up, u);
				to_camera(host, s.dir, d);
				set_float3(runtime, "SunVCenter", c[0], c[1], c[2]);
				set_float3(runtime, "SunVRight", r[0], r[1], r[2]);
				set_float3(runtime, "SunVUp", u[0], u[1], u[2]);
				set_float3(runtime, "SunVDir", d[0], d[1], d[2]);
				set_float3(runtime, "SunVParams", s.half, s.near_dist, float(g_sunN));
			}
			set_bool(runtime, "SunVActive", sun_map);
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "AstraShadow"); v.handle != 0)
				runtime->set_uniform_value_float(v, sun_map ? shadow : 0.0f);
		}

		void on_reloaded_effects(effect_runtime *runtime)
		{
			bind(runtime);
		}

		void on_render_technique(effect_runtime *runtime, effect_technique technique, command_list *, resource_view, resource_view)
		{
			char name[64] = "";
			runtime->get_technique_effect_name(technique, name);
			if (std::strcmp(name, kEffect) == 0)
				++g_herRuns;
		}

		void on_destroy_effect_runtime(effect_runtime *runtime)
		{
			runtime->get_command_queue()->wait_idle(); // her textures and upload buffers may be in a frame in flight
			destroy_layers(runtime->get_device());
		}
	}

	void install()
	{
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::reshade_render_technique>(on_render_technique);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
	}

	void set_active(bool active)
	{
		g_active = active;
	}

	bool active()
	{
		return g_active;
	}

	void set_hdr_pass(bool hdr, unsigned long long frame)
	{
		g_passFrame = frame;
		g_hdrPass = hdr;
	}

	bool tone_ready()
	{
		return g_toneLut.tex.handle != 0;
	}

	float &tone_exposure()
	{
		return g_toneExposure;
	}

	unsigned long long her_runs()
	{
		return g_herRuns;
	}

	void set_frames_at(const char *where)
	{
		// "/dev/shm/name" (Linux: the file, seen by Wine under Z:) or a Win32 mapping name ("Local\\name")
		wchar_t name[128] = L"", path[260] = L"";
		if (where[0] == '/')
		{
			const int n = std::swprintf(path, 260, L"Z:%hs", where);
			for (int i = 0; i < n; ++i)
				if (path[i] == L'/')
					path[i] = L'\\';
			std::swprintf(name, 128, L"Local\\%hs", std::strrchr(where, '/') + 1);
		}
		else
			std::swprintf(name, 128, L"%hs", where);
		wcscpy_s(g_name, name);
		wcscpy_s(g_path, path);
		g_named = true;
		close_mapping(); // at every hello, even the same name: a restarted engine's ring is a new file
		g_nextOpen = 0;
		std::snprintf(g_status, sizeof(g_status), "ring: %s (not mapped yet)", where);
		logf("layer: the engine's frames are at %s", where);
	}

	void set_host(const view::Pose &host, float near_m, float far_m)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_host = host;
		g_hostNear = near_m > 0.0f ? near_m : 0.1f;
		g_hostFar = far_m;
	}

	void set_shadow(float strength)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_shadow = strength;
	}

	void expect_frame(unsigned long long cam_id)
	{
		g_wantFrame = cam_id;
	}

	float &wait_ms()
	{
		return g_waitMs;
	}

	unsigned long long wait_timeouts()
	{
		return g_waitTimeouts;
	}

	unsigned long long frames()
	{
		return g_frames;
	}

	const char *status()
	{
		return g_status;
	}
}
