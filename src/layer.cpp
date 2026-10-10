#include "layer.h"
#include "guard.h"
#include "link.h"
#include "log.h"
#include "ring.h"
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
		constexpr float kHostFar = 1.0e7f; // Unreal's projection has no far plane: reversed Z, infinite

		struct Layer
		{
			resource tex = {0};
			resource_view srv = {0};
		};

		std::atomic<bool> g_active{false};
		std::mutex g_lock; // host pose, shadow strength (written by the link on the present thread; the effects may run elsewhere)
		view::Pose g_host;
		float g_hostNear = 0.1f;
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

		void destroy_layers(device *dev)
		{
			destroy(dev, g_world);
			destroy(dev, g_depth);
			destroy(dev, g_sunMap);
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

		void bind(effect_runtime *runtime)
		{
			runtime->update_texture_bindings("ASTRAWORLD", g_world.srv, g_world.srv);
			runtime->update_texture_bindings("ASTRADEPTH", g_depth.srv, g_depth.srv);
			runtime->update_texture_bindings("ASTRASUN", g_sunMap.srv, g_sunMap.srv);
		}

		void upload_plane(device *dev, Layer &l, const std::vector<uint8_t> &data, uint32_t w, uint32_t h)
		{
			subresource_data d;
			d.data = const_cast<uint8_t *>(data.data());
			d.row_pitch = w * 4;
			d.slice_pitch = w * h * 4;
			dev->update_texture_region(d, l.tex, 0);
		}

		/// The newest published frame, if not shown yet.
		void upload(effect_runtime *runtime)
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
			g_lastPublish = f.publish;
			device *dev = runtime->get_device();
			if (!f.empty)
			{
				if (f.rw != g_texW || f.rh != g_texH)
				{
					destroy(dev, g_world);
					destroy(dev, g_depth);
					g_texW = g_texH = 0;
					if (!create_layer(dev, g_world, f.rw, f.rh, format::r8g8b8a8_unorm) || !create_layer(dev, g_depth, f.rw, f.rh, format::r32_float))
					{
						destroy(dev, g_world);
						destroy(dev, g_depth);
						return;
					}
					g_texW = f.rw;
					g_texH = f.rh;
					bind(runtime);
				}
				upload_plane(dev, g_world, g_colourCopy, f.rw, f.rh);
				upload_plane(dev, g_depth, g_depthCopy, f.rw, f.rh);
				if (f.sun.valid)
				{
					if (f.sun.n != g_sunN)
					{
						destroy(dev, g_sunMap);
						g_sunN = create_layer(dev, g_sunMap, f.sun.n, f.sun.n, format::r32_float) ? f.sun.n : 0;
						bind(runtime);
					}
					if (g_sunN == f.sun.n)
						upload_plane(dev, g_sunMap, g_sunCopy, f.sun.n, f.sun.n);
				}
			}
			g_frame = f;
			g_hasFrame = true;
			++g_frames;
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

		void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
		{
			bool on = g_active && open_mapping();
			if (on)
			{
				wait_for_frame();
				upload(runtime);
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
			float host_near, shadow;
			{
				std::lock_guard<std::mutex> lock(g_lock);
				host = g_host;
				host_near = g_hostNear;
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
			set_float3(runtime, "HostPlanes", host_near, kHostFar, 0.0f);
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

		void on_destroy_effect_runtime(effect_runtime *runtime)
		{
			destroy_layers(runtime->get_device());
		}
	}

	void install()
	{
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
	}

	void set_active(bool active)
	{
		g_active = active;
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

	void set_host(const view::Pose &host, float near_m)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_host = host;
		g_hostNear = near_m > 0.0f ? near_m : 0.1f;
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
