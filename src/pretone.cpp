#include "pretone.h"
#include "layer.h"
#include "log.h"
#include "passfind.h"
#include "pretone_rules.h"
#include "provider.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <algorithm>
#include <iterator>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <reshade.hpp>

using namespace reshade::api;

namespace pretone
{
	namespace
	{
		/// One command list: what the pass finder saw, and the graphics state the game bound (put back after her pass:
		/// ReShade's own state capture does nothing on Direct3D 12).
		struct __declspec(uuid("7c3f5a52-2d1e-4c8b-9e64-5b1a0f3d2e71")) ListState
		{
			passfind::Tracker find;
			uint64_t epoch = 0;        // g_onEpoch when this list was reset with pretone on (0: not tracked from its start)
			pretone_rules::Mark mark; // where its previous draw left the recording thread's descriptor copies
			std::vector<resource_view> rtvs;
			resource_view dsv = {0};
			pipeline pso = {0};
			pipeline_stage pso_stages = pipeline_stage::all;
			bool has_topology = false, has_blend = false, has_sample_mask = false, has_stencil = false;
			uint32_t topology = 0, blend = 0, sample_mask = 0xFFFFFFFF, stencil_front = 0, stencil_back = 0;
			std::vector<viewport> viewports;
			std::vector<rect> scissors;
			enum class Arg : uint8_t
			{
				none,
				table,
				cbv,
				srv,
				uav,
				constants,
			};
			struct Param
			{
				Arg kind = Arg::none;
				descriptor_table table = {0};
				buffer_range cbv = {};
				uint64_t va = 0; // a root SRV/UAV's GPU address
				std::vector<uint32_t> constants;
			};

			/// A root signature and its arguments (the graphics one, and the compute one).
			struct Root
			{
				pipeline_layout layout = {0};
				std::vector<Param> params;
				/// The argument `index` of root signature `l` (a new one drops them all, as Direct3D does).
				Param &param(pipeline_layout l, uint32_t index)
				{
					if (l != layout)
					{
						params.clear();
						layout = l;
					}
					if (params.size() <= index)
						params.resize(index + 1);
					return params[index];
				}
				void clear()
				{
					layout = {0};
					params.clear();
				}
			};
			Root gfx, cmp;

			void clear()
			{
				find.reset();
				epoch = 0;
				mark = pretone_rules::Mark();
				rtvs.clear();
				dsv = {0};
				pso = {0};
				has_topology = has_blend = has_sample_mask = has_stencil = false;
				viewports.clear();
				scissors.clear();
				gfx.clear();
				cmp.clear();
			}
			Root *root(shader_stage stages)
			{
				if ((stages & shader_stage::all_compute) != 0)
					return (stages & shader_stage::all_graphics) == 0 ? &cmp : nullptr;
				return (stages & shader_stage::all_graphics) != 0 ? &gfx : nullptr;
			}
		};

		void (*g_beginFrame)() = nullptr;
		std::atomic<effect_runtime *> g_runtime{nullptr};
		std::atomic<uint32_t> g_outW{0}, g_outH{0};
		std::atomic<bool> g_hdrOutput{false};
		std::atomic<uint64_t> g_presents{0}, g_drawnAt{~uint64_t(0)}, g_ranAt{~uint64_t(0)}, g_foundAt{~uint64_t(0)};
		int g_path = 0;
		std::string g_iniEngine; // [tone] Engine= of the launcher's ini

		// pretone_rules::choose, once per Present: on only in a game declared Unreal. Off, every command-stream event
		// below returns at once - the game's frames are handled exactly as before pretone existed. A list is tracked
		// only if pretone was on since its reset (g_onEpoch): what it bound before is unknown, and her pass puts back
		// what it saw bound.
		std::atomic<bool> g_on{false};
		std::atomic<uint64_t> g_onEpoch{1};
		const char *g_why = "";

		// ReShade picks her effects' target by whether ANY thread is inside Present (a runtime-wide flag): a pass drawn
		// on a recording thread while the present thread is in ReShade's Present would draw into the back buffer's
		// copy, and both would race ReShade's per-frame state. The present thread holds g_presenting from the add-on's
		// present event until reshade_present (and waits there for a pass in flight); a recording thread draws her pass
		// only while it holds g_recording - one at a time - and Present is not in progress (else: Present path).
		std::atomic<bool> g_presenting{false}, g_recording{false};

		// her pass's own target: a copy of the game's scene colour (same size and format); a replaced one is destroyed
		// only frames later (the GPU may still read it)
		struct Copy
		{
			resource tex = {0};
			resource_view rtv = {0};
		};
		std::mutex g_targetLock;
		device *g_device = nullptr;
		resource g_copy = {0};
		resource_view g_copyRtv = {0};
		resource_desc g_copyDesc;
		pretone_rules::Retired<Copy> g_retired;
		pretone_rules::Fallback g_fallback;   // touched only by the thread holding g_recording
		pretone_rules::Stability g_stability; // touched only by the thread holding g_recording

		// what happened, for the log and the overlay
		std::atomic<uint64_t> g_before{0}, g_atPresent{0}, g_lost{0}, g_found{0}, g_contended{0}, g_periodBefore{0}, g_periodPresent{0}, g_periodLost{0},
			g_deferred{0};
		std::atomic<uint32_t> g_api{0}; // the device_api of the tonemapper pass last driven (0: none yet)
		ULONGLONG g_nextStats = 0;
		bool g_loggedFound = false, g_loggedNotMain = false, g_loggedAmbiguous = false, g_loggedCompute = false, g_loggedHdr = false, g_loggedNone = false, g_loggedApi = false,
			 g_loggedFail = false, g_loggedHold = false, g_loggedGiveUp = false, g_loggedDeferred = false, g_loggedResChanges = false;
		char g_status[260] = "drawn at presentation (over the game's UI): no frame yet";

		// ASTRA_PRETONE_TRACE=<frame>: that frame's command stream as the finder sees it, into the log (a game whose
		// tonemapper is not found: what its post-processing looks like)
		uint64_t g_traceFrame = ~uint64_t(0);
		std::atomic<int> g_traceLines{0};

		bool tracing()
		{
			const uint64_t f = g_presents.load();
			return f >= g_traceFrame && f <= g_traceFrame + 3 && g_traceLines < 2000; // a few frames: recording overlaps presents
		}

		uint32_t format_of(device *dev, uint64_t id)
		{
			return id != 0 ? uint32_t(dev->get_resource_desc(resource{id}).texture.format) : 0;
		}

		/// For the log and the overlay: which API path is driving her pass (item 2 of the D3D11 port - every message
		/// that names a path must say which one).
		const char *api_name(device_api api)
		{
			switch (api)
			{
			case device_api::d3d11:
				return "Direct3D 11";
			case device_api::d3d12:
				return "Direct3D 12";
			default:
				return "another API";
			}
		}

		ListState *state(command_list *cmd)
		{
			return cmd->get_private_data<ListState>();
		}

		passfind::Tex describe(device *dev, resource r)
		{
			passfind::Tex t;
			if (r.handle == 0)
				return t;
			const resource_desc d = dev->get_resource_desc(r);
			if (d.type != resource_type::texture_2d || d.texture.samples != 1 || d.texture.depth_or_layers != 1)
				return t;
			t.id = r.handle;
			t.w = d.texture.width;
			t.h = d.texture.height;
			t.kind = passfind::kind_of(uint32_t(d.texture.format));
			return t;
		}

		/// The recording thread's descriptor copies (pretone_rules::CopyRing).
		thread_local pretone_rules::CopyRing t_ring;

		/// A list starts (made, or reset): tracked from here if pretone is on.
		void start_list(ListState &s)
		{
			s.clear();
			s.epoch = g_on.load(std::memory_order_relaxed) ? g_onEpoch.load(std::memory_order_relaxed) : 0;
			s.mark = pretone_rules::Mark{&t_ring, t_ring.next};
		}

		bool on()
		{
			return g_on.load(std::memory_order_relaxed);
		}

		void on_init_command_list(command_list *cmd)
		{
			if (ListState *s = cmd->create_private_data<ListState>())
				start_list(*s);
		}

		void on_destroy_command_list(command_list *cmd)
		{
			cmd->destroy_private_data<ListState>();
		}

		void on_reset_command_list(command_list *cmd)
		{
			if (ListState *s = state(cmd))
				start_list(*s);
		}

		void on_bind_render_targets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
		{
			if (!on())
				return;
			ListState *s = state(cmd);
			if (s == nullptr)
				return;
			s->rtvs.assign(rtvs, rtvs + count);
			s->dsv = dsv;
			passfind::Tex first;
			if (count >= 1 && rtvs[0].handle != 0)
			{
				device *dev = cmd->get_device();
				first = describe(dev, dev->get_resource_from_view(rtvs[0]));
			}
			s->find.targets(count, first);
		}

		bool on_begin_render_pass(command_list *cmd, uint32_t, const render_pass_render_target_desc *, const render_pass_depth_stencil_desc *, render_pass_flags)
		{
			if (!on())
				return false;
			if (ListState *s = state(cmd))
				s->find.in_pass = true;
			return false;
		}

		bool on_end_render_pass(command_list *cmd)
		{
			if (!on())
				return false;
			if (ListState *s = state(cmd))
				s->find.in_pass = false;
			return false;
		}

		void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline p)
		{
			if (!on())
				return;
			ListState *s = state(cmd);
			if (s != nullptr && (stages & (pipeline_stage::all_graphics | pipeline_stage::all_compute)) != 0)
			{
				s->pso = p; // Direct3D 12 has one pipeline state slot: the last one bound, graphics or compute
				s->pso_stages = stages;
			}
		}

		void on_bind_pipeline_states(command_list *cmd, uint32_t count, const dynamic_state *states, const uint32_t *values)
		{
			if (!on())
				return;
			ListState *s = state(cmd);
			if (s == nullptr)
				return;
			for (uint32_t i = 0; i < count; ++i)
				switch (states[i])
				{
				case dynamic_state::primitive_topology:
					s->has_topology = true;
					s->topology = values[i];
					break;
				case dynamic_state::blend_constant:
					s->has_blend = true;
					s->blend = values[i];
					break;
				case dynamic_state::sample_mask:
					s->has_sample_mask = true;
					s->sample_mask = values[i];
					break;
				case dynamic_state::front_stencil_reference_value:
					s->has_stencil = true;
					s->stencil_front = values[i];
					break;
				case dynamic_state::back_stencil_reference_value:
					s->has_stencil = true;
					s->stencil_back = values[i];
					break;
				default:
					break;
				}
		}

		void on_bind_viewports(command_list *cmd, uint32_t first, uint32_t count, const viewport *vps)
		{
			if (!on())
				return;
			ListState *s = state(cmd);
			if (s == nullptr)
				return;
			if (s->viewports.size() < first + count)
				s->viewports.resize(first + count);
			for (uint32_t i = 0; i < count; ++i)
				s->viewports[first + i] = vps[i];
			if (first == 0 && count > 0)
				s->find.viewport(vps[0].x, vps[0].y, vps[0].width, vps[0].height);
		}

		void on_bind_scissor_rects(command_list *cmd, uint32_t first, uint32_t count, const rect *rects)
		{
			if (!on())
				return;
			ListState *s = state(cmd);
			if (s == nullptr)
				return;
			if (s->scissors.size() < first + count)
				s->scissors.resize(first + count);
			for (uint32_t i = 0; i < count; ++i)
				s->scissors[first + i] = rects[i];
		}

		void on_bind_descriptor_tables(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
			const descriptor_table *tables, uint32_t, const uint32_t *)
		{
			if (!on())
				return;
			ListState *s = state(cmd);
			ListState::Root *r = s != nullptr ? s->root(stages) : nullptr;
			if (r == nullptr)
				return;
			if (layout != r->layout)
			{
				r->params.clear(); // Set*RootSignature (count 0) or a table of another one
				r->layout = layout;
			}
			for (uint32_t i = 0; i < count; ++i)
			{
				ListState::Param &p = r->param(layout, first + i);
				p.kind = ListState::Arg::table;
				p.table = tables[i];
			}
		}

		/// D3D11: what a draw (or, `compute`, a dispatch) reads right now, asked of the context itself. Called only at a
		/// candidate draw (a full-screen shape, or a traced one), so nothing is tracked per *SetShaderResources call; an
		/// empty slot is skipped before anything touches it (ReShade's get_resource_from_view dereferences a null view);
		/// a bound view is alive while it is read (the context holds it). A D3D11 resource's ReShade handle IS its
		/// ID3D11Resource pointer, the same one on_bind_render_targets' get_resource_from_view gives.
		size_t d3d11_reads(command_list *cmd, bool compute, passfind::Tex *out, size_t cap)
		{
			auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
			device *dev = cmd->get_device();
			size_t n = 0;
			const auto take = [&](ID3D11View *view, bool written) {
				if (view == nullptr)
					return;
				ID3D11Resource *res = nullptr;
				if (n < cap)
					view->GetResource(&res);
				if (res != nullptr)
				{
					passfind::Tex t = describe(dev, resource{uint64_t(uintptr_t(res))});
					res->Release();
					if (t.id != 0)
					{
						t.written = written;
						out[n++] = t;
					}
				}
				view->Release();
			};
			ID3D11ShaderResourceView *srv[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
			if (compute)
				ctx->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srv);
			else
				ctx->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srv);
			for (ID3D11ShaderResourceView *v : srv)
				take(v, false);
			if (compute)
			{
				ID3D11UnorderedAccessView *uav[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
				ctx->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, uav);
				for (ID3D11UnorderedAccessView *v : uav)
					take(v, true);
			}
			return n;
		}

		void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t index, const descriptor_table_update &update)
		{
			if (!on())
				return;
			if (cmd->get_device()->get_api() == device_api::d3d11)
				return; // D3D11: what a draw reads is asked of the context at a candidate draw (d3d11_reads)
			ListState *s = state(cmd);
			ListState::Root *r = s != nullptr ? s->root(stages) : nullptr;
			if (r == nullptr || update.count != 1 || update.descriptors == nullptr)
				return;
			ListState::Param &p = r->param(layout, index);
			switch (update.type)
			{
			case descriptor_type::constant_buffer: // ReShade resolved the address into a buffer and an offset
				p.kind = ListState::Arg::cbv;
				p.cbv = *static_cast<const buffer_range *>(update.descriptors);
				break;
			case descriptor_type::buffer_shader_resource_view: // the raw GPU address (Direct3D 12's root SRV)
				p.kind = ListState::Arg::srv;
				p.va = *static_cast<const uint64_t *>(update.descriptors);
				break;
			case descriptor_type::buffer_unordered_access_view:
				p.kind = ListState::Arg::uav;
				p.va = *static_cast<const uint64_t *>(update.descriptors);
				break;
			default:
				break;
			}
		}

		void on_push_constants(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t index, uint32_t first, uint32_t count, const void *values)
		{
			if (!on())
				return;
			ListState *s = state(cmd);
			ListState::Root *r = s != nullptr ? s->root(stages) : nullptr;
			if (r == nullptr || values == nullptr)
				return;
			ListState::Param &p = r->param(layout, index);
			if (p.kind != ListState::Arg::constants)
				p.constants.clear();
			p.kind = ListState::Arg::constants;
			if (p.constants.size() < first + count)
				p.constants.resize(first + count);
			std::memcpy(p.constants.data() + first, values, count * sizeof(uint32_t));
		}

		// the shader-resource state each HDR texture was last put in (recording order across threads is not the GPU's,
		// but a texture the tonemapper reads is in one of these): her pass puts the scene colour back in it
		std::mutex g_stateLock;
		std::unordered_map<uint64_t, uint32_t> g_readState;

		void on_barrier(command_list *cmd, uint32_t count, const resource *resources, const resource_usage *, const resource_usage *new_states)
		{
			if (!on())
				return;
			constexpr resource_usage writes = resource_usage::render_target | resource_usage::unordered_access | resource_usage::depth_stencil_write |
											  resource_usage::copy_dest | resource_usage::vertex_buffer;
			for (uint32_t i = 0; i < count; ++i)
			{
				if (resources[i].handle == 0 || (new_states[i] & resource_usage::shader_resource) == 0 || (new_states[i] & writes) != 0)
					continue;
				const passfind::Tex t = describe(cmd->get_device(), resources[i]);
				// a candidate scene colour: the presented size, or a sub-resolution one (phase B) - the wider net
				// (trace_candidate_size) errs toward tracking a few extra HDR textures, never toward missing the real one
				if (t.kind != passfind::Kind::hdr || !passfind::trace_candidate_size(t.w, t.h, g_outW, g_outH))
					continue;
				std::lock_guard<std::mutex> lock(g_stateLock);
				if (g_readState.size() > 4096)
					g_readState.clear(); // resources come and go: never grows without bound
				g_readState[t.id] = uint32_t(new_states[i]);
			}
		}

		/// The state to take the scene colour out of: the last shader-resource one seen - with the pixel stage for a
		/// pixel shader's read - else all shader stages.
		resource_usage read_state(uint64_t id, bool pixel)
		{
			std::lock_guard<std::mutex> lock(g_stateLock);
			const auto it = g_readState.find(id);
			const uint32_t st = it != g_readState.end() ? it->second : 0;
			const uint32_t need = pixel ? uint32_t(resource_usage::shader_resource_pixel) : uint32_t(resource_usage::shader_resource);
			return (st & need) != 0 ? resource_usage(st) : resource_usage::shader_resource;
		}

		// What a draw reads: Direct3D 12 games copy the descriptors of a draw's textures into the shader-visible heap
		// right before it, on the thread recording it (Unreal's descriptor cache does). The thread's ring of copies maps a
		// bound table's slots back to the views copied there - only the copies since the list's previous draw
		// (pretone_rules::window) - and the views to their textures.

		// Which slots of each root signature's tables are written (unordered-access views): a compute tonemapper writes
		// its output through one. Root signatures are made at load; looked up only for a draw or dispatch of interest.
		struct Range
		{
			uint32_t offset, count; // in the table (count ~0u: unbounded)
			bool written;
		};
		std::shared_mutex g_layoutLock;
		std::unordered_map<uint64_t, std::vector<std::vector<Range>>> g_layouts; // root signature -> parameter -> ranges

		void on_init_pipeline_layout(device *dev, uint32_t count, const pipeline_layout_param *params, pipeline_layout layout)
		{
			if (dev->get_api() != device_api::d3d12 || layout.handle == 0)
				return;
			std::vector<std::vector<Range>> ps(count);
			for (uint32_t i = 0; i < count; ++i)
			{
				const pipeline_layout_param &p = params[i];
				const bool flags = p.type == pipeline_layout_param_type::descriptor_table_with_flags;
				if (!flags && p.type != pipeline_layout_param_type::descriptor_table)
					continue;
				const uint32_t n = flags ? p.descriptor_table_with_flags.count : p.descriptor_table.count;
				for (uint32_t k = 0; k < n; ++k)
				{
					const descriptor_range &r = flags ? static_cast<const descriptor_range &>(p.descriptor_table_with_flags.ranges[k]) : p.descriptor_table.ranges[k];
					const bool uav = r.type == descriptor_type::unordered_access_view || r.type == descriptor_type::buffer_unordered_access_view;
					const bool srv = r.type == descriptor_type::shader_resource_view || r.type == descriptor_type::buffer_shader_resource_view;
					if (uav || srv)
						ps[i].push_back(Range{r.binding, r.count, uav});
				}
			}
			std::unique_lock<std::shared_mutex> lock(g_layoutLock);
			g_layouts[layout.handle] = std::move(ps);
		}

		void on_destroy_pipeline_layout(device *, pipeline_layout layout)
		{
			std::unique_lock<std::shared_mutex> lock(g_layoutLock);
			g_layouts.erase(layout.handle);
		}

		/// Slot `slot` of table parameter `param`: written (a UAV)?
		bool slot_written(const std::vector<std::vector<Range>> *ranges, size_t param, uint32_t slot)
		{
			if (ranges == nullptr || param >= ranges->size())
				return false;
			for (const Range &r : (*ranges)[param])
				if (slot >= r.offset && (r.count == ~0u || slot < r.offset + r.count))
					return r.written;
			return false;
		}
		std::atomic<uint32_t> g_increment{0}; // the CBV/SRV/UAV descriptor size

		bool on_copy_descriptor_tables(device *dev, uint32_t count, const descriptor_table_copy *copies)
		{
			if (!on() || dev->get_api() != device_api::d3d12)
				return false;
			uint32_t inc = g_increment;
			if (inc == 0)
			{
				inc = reinterpret_cast<ID3D12Device *>(dev->get_native())->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				g_increment = inc;
			}
			constexpr uint64_t cpu = 0xF000000000000000ull; // ReShade's mark of a CPU descriptor handle as a table
			for (uint32_t i = 0; i < count; ++i)
			{
				const descriptor_table_copy &c = copies[i];
				// CPU to CPU (into a shader-visible heap), CBV/SRV/UAV only (the handle's low bits are its heap type)
				if ((c.source_table.handle & cpu) != cpu || (c.dest_table.handle & cpu) != cpu || (c.source_table.handle & 0x3) != 0 || c.count == 0)
					continue;
				descriptor_heap heap = {0}, src_heap = {0};
				uint32_t off = 0, src_off = 0;
				dev->get_descriptor_heap_offset(c.dest_table, c.dest_binding, 0, &heap, &off);
				dev->get_descriptor_heap_offset(c.source_table, c.source_binding, 0, &src_heap, &src_off);
				t_ring.push(pretone_rules::Copy{heap.handle, off, c.count, src_heap.handle, src_off});
			}
			return false;
		}

		/// The textures the draw about to be recorded reads, through the tables bound for it: only the descriptors copied
		/// since this list's previous draw (`window`), each looked up through ReShade's view of it.
		size_t draw_reads(command_list *cmd, const ListState::Root &s, uint32_t window, passfind::Tex *out, size_t cap)
		{
			device *dev = cmd->get_device();
			const uint32_t inc = g_increment;
			if (inc == 0 || window == 0)
				return 0;
			std::vector<std::vector<Range>> ranges; // this root signature's table ranges (a copy: the lock is not held long)
			{
				std::shared_lock<std::shared_mutex> lock(g_layoutLock);
				if (const auto it = g_layouts.find(s.layout.handle); it != g_layouts.end())
					ranges = it->second;
			}
			constexpr size_t kTables = 16;
			pretone_rules::Table tables[kTables];
			const size_t n_tables = std::min(s.params.size(), kTables);
			for (size_t pi = 0; pi < n_tables; ++pi)
				if (s.params[pi].kind == ListState::Arg::table && s.params[pi].table.handle != 0)
				{
					descriptor_heap heap = {0};
					dev->get_descriptor_heap_offset(s.params[pi].table, 0, 0, &heap, &tables[pi].base);
					tables[pi].heap = heap.handle;
				}
			pretone_rules::Slot slots[kTables * pretone_rules::kSpan];
			const size_t n_slots = pretone_rules::resolve(t_ring, window, tables, n_tables, slots, kTables * pretone_rules::kSpan);
			size_t n = 0;
			for (size_t i = 0; i < n_slots; ++i)
			{
				const pretone_rules::Slot &sl = slots[i];
				// the view's original CPU handle: its heap's start + its slot (the heap is the original object)
				const D3D12_CPU_DESCRIPTOR_HANDLE start = reinterpret_cast<ID3D12DescriptorHeap *>(sl.src_heap)->GetCPUDescriptorHandleForHeapStart();
				const resource r = dev->get_resource_from_view(resource_view{start.ptr + uint64_t(sl.src_off) * inc});
				passfind::Tex t = describe(dev, r);
				if (t.id == 0)
					continue;
				t.written = slot_written(&ranges, sl.table, sl.slot);
				bool dup = false;
				for (size_t q = 0; q < n; ++q)
					if (out[q].id == t.id)
					{
						dup = true;
						out[q].written = out[q].written || t.written;
					}
				if (!dup && n < cap)
					out[n++] = t;
			}
			return n;
		}

		void destroy_now(const Copy &c)
		{
			if (g_device == nullptr)
				return;
			if (c.rtv.handle != 0)
				g_device->destroy_resource_view(c.rtv);
			if (c.tex.handle != 0)
				g_device->destroy_resource(c.tex);
		}

		/// The copy in use is replaced: destroyed only once no frame in flight can read it (pretone_rules::Retired).
		void retire_copy(uint64_t frame)
		{
			if (g_copy.handle != 0 || g_copyRtv.handle != 0)
				g_retired.retire(Copy{g_copy, g_copyRtv}, frame);
			g_copy = {0};
			g_copyRtv = {0};
		}

		/// Her pass's target: the tonemapper's IMAGE (its viewport `vp_w` x `vp_h`, never the padded target it may sit
		/// inside - a pool's padding is never drawn), like the game's scene colour in format, and writable by ReShade.
		/// `vp_w`/`vp_h` 0 (or bigger than the scene itself - never asked for, a defensive fallback): the scene's own size.
		bool ensure_copy(device *dev, resource scene, uint32_t vp_w, uint32_t vp_h, uint64_t frame)
		{
			const resource_desc d = dev->get_resource_desc(scene);
			const format typed = format(passfind::typed_hdr(uint32_t(d.texture.format)));
			const uint32_t w = vp_w != 0 && vp_w <= d.texture.width ? vp_w : d.texture.width;
			const uint32_t h = vp_h != 0 && vp_h <= d.texture.height ? vp_h : d.texture.height;
			if (g_copy.handle != 0 && g_device == dev && g_copyDesc.texture.width == w && g_copyDesc.texture.height == h &&
				g_copyDesc.texture.format == typed)
				return true;
			if (g_device != dev)
			{
				retire_copy(0); // another device: its runtime went (and waited for its GPU)
				g_retired.collect(~uint64_t(0) >> 1, destroy_now);
			}
			retire_copy(frame);
			g_device = dev;
			g_copyDesc = resource_desc(w, h, 1, 1, typed, 1, memory_heap::default_,
				resource_usage::render_target | resource_usage::shader_resource | resource_usage::copy_source | resource_usage::copy_dest);
			if (!dev->create_resource(g_copyDesc, nullptr, resource_usage::shader_resource, &g_copy))
			{
				g_copy = {0};
				return false;
			}
			dev->set_resource_name(g_copy, "Astra: the game's scene colour, her pass");
			if (!dev->create_resource_view(g_copy, resource_usage::render_target, resource_view_desc(typed), &g_copyRtv))
			{
				dev->destroy_resource(g_copy); // never used by the GPU
				g_copy = {0};
				g_copyRtv = {0};
				return false;
			}
			return true;
		}

		/// One root signature's arguments back (ReShade's effects set their own heaps and root signatures, which leaves
		/// the game's bound tables undefined - graphics AND compute).
		void restore_root(command_list *cmd, shader_stage stage, const ListState::Root &r)
		{
			if (r.layout.handle == 0)
				return;
			// no tables: ReShade puts the game's descriptor heaps and this root signature back
			cmd->bind_descriptor_tables(stage, r.layout, 0, 0, nullptr);
			const bool compute = stage == shader_stage::all_compute;
			auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
			for (uint32_t i = 0; i < uint32_t(r.params.size()); ++i)
			{
				const ListState::Param &p = r.params[i];
				switch (p.kind)
				{
				case ListState::Arg::table:
					cmd->bind_descriptor_tables(stage, r.layout, i, 1, &p.table);
					break;
				case ListState::Arg::cbv:
					cmd->push_descriptors(stage, r.layout, i, descriptor_table_update{{}, 0, 0, 1, descriptor_type::constant_buffer, &p.cbv});
					break;
				case ListState::Arg::srv: // ReShade's push_descriptors cannot take a raw address back: native
					compute ? native->SetComputeRootShaderResourceView(i, p.va) : native->SetGraphicsRootShaderResourceView(i, p.va);
					break;
				case ListState::Arg::uav:
					compute ? native->SetComputeRootUnorderedAccessView(i, p.va) : native->SetGraphicsRootUnorderedAccessView(i, p.va);
					break;
				case ListState::Arg::constants:
					cmd->push_constants(stage, r.layout, i, 0, uint32_t(p.constants.size()), p.constants.data());
					break;
				case ListState::Arg::none:
					break;
				}
			}
		}

		/// The game's state back, as it was bound before her pass (the tonemapper draws right after): both root
		/// signatures with their arguments, the pipeline, the dynamic states, viewports, scissors, targets.
		void restore(command_list *cmd, const ListState &s)
		{
			restore_root(cmd, shader_stage::all_compute, s.cmp);
			restore_root(cmd, shader_stage::all_graphics, s.gfx); // last: its heaps are the ones the next draw uses
			if (s.pso.handle != 0)
				cmd->bind_pipeline(s.pso_stages, s.pso); // the slot is shared, the game's next draw or dispatch uses it
			if (s.has_topology)
				cmd->bind_pipeline_state(dynamic_state::primitive_topology, s.topology);
			if (s.has_blend)
				cmd->bind_pipeline_state(dynamic_state::blend_constant, s.blend);
			if (s.has_sample_mask)
				cmd->bind_pipeline_state(dynamic_state::sample_mask, s.sample_mask);
			if (s.has_stencil)
			{
				cmd->bind_pipeline_state(dynamic_state::front_stencil_reference_value, s.stencil_front);
				cmd->bind_pipeline_state(dynamic_state::back_stencil_reference_value, s.stencil_back);
			}
			if (!s.viewports.empty())
				cmd->bind_viewports(0, uint32_t(s.viewports.size()), s.viewports.data());
			if (!s.scissors.empty())
				cmd->bind_scissor_rects(0, uint32_t(s.scissors.size()), s.scissors.data());
			cmd->bind_render_targets_and_depth_stencil(uint32_t(s.rtvs.size()), s.rtvs.data(), s.dsv);
		}

		// ---- D3D11: the whole context state, saved and restored around her pass --------------------------------
		//
		// ReShade's effect_runtime::render_effects does not save or restore ANY D3D11 context state (unlike its own
		// Present path, which wraps it in a private state block) - so unlike D3D12's restore() above (built by
		// TRACKING what the game bound, since D3D12 has no "get the current state back" call), this reads the real
		// thing with the context's own Get* calls right before render_effects runs, and puts it back with the
		// matching Set* right after. Modelled on the field list of ReShade's own d3d11_impl_state_block (read on
		// GitHub) - this is new code, not a copy of it.

		/// One shader stage's shader + class instances + constant buffers + SRVs + samplers (VS/HS/DS/GS/PS; CS adds
		/// UAVs and is captured separately below). Every pointer a Get* call hands back is a NEW reference - the
		/// destructor releases all of them.
		template <class Shader>
		struct D3D11StageState
		{
			// D3D11_SHADER_MAX_INTERFACES is 253; HLSL dynamic shader linkage is rare enough that a game using more
			// than 32 class instances on one stage is UNVERIFIED territory for this phase.
			static constexpr UINT kClass = 32;
			static constexpr UINT kCb = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
			static constexpr UINT kSrv = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
			static constexpr UINT kSampler = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;

			Shader *shader = nullptr;
			ID3D11ClassInstance *instances[kClass] = {};
			UINT instance_count = kClass;
			ID3D11Buffer *cb[kCb] = {};
			ID3D11ShaderResourceView *srv[kSrv] = {};
			ID3D11SamplerState *sampler[kSampler] = {};

			D3D11StageState() = default;
			D3D11StageState(const D3D11StageState &) = delete;
			D3D11StageState &operator=(const D3D11StageState &) = delete;
			~D3D11StageState()
			{
				if (shader != nullptr)
					shader->Release();
				for (ID3D11ClassInstance *p : instances)
					if (p != nullptr)
						p->Release();
				for (ID3D11Buffer *p : cb)
					if (p != nullptr)
						p->Release();
				for (ID3D11ShaderResourceView *p : srv)
					if (p != nullptr)
						p->Release();
				for (ID3D11SamplerState *p : sampler)
					if (p != nullptr)
						p->Release();
			}
		};

// VS/HS/DS/GS/PS share one shape (shader + class instances, constant buffers, SRVs, samplers); CS's own capture/
// restore (below) adds its UAVs. A template helper over the method name would need as much machinery as this macro
// to stay type-correct across 5 unrelated COM interfaces - six near-identical blocks, kept duplicated on purpose.
#define ASTRA_D3D11_STAGE(TAG, Shader)                                                                                                             \
		void d3d11_capture_##TAG(ID3D11DeviceContext *ctx, D3D11StageState<Shader> &s)                                                             \
		{                                                                                                                                           \
			s.instance_count = D3D11StageState<Shader>::kClass;                                                                                    \
			ctx->TAG##GetShader(&s.shader, s.instances, &s.instance_count);                                                                        \
			ctx->TAG##GetConstantBuffers(0, D3D11StageState<Shader>::kCb, s.cb);                                                                   \
			ctx->TAG##GetShaderResources(0, D3D11StageState<Shader>::kSrv, s.srv);                                                                 \
			ctx->TAG##GetSamplers(0, D3D11StageState<Shader>::kSampler, s.sampler);                                                                 \
		}                                                                                                                                           \
		void d3d11_restore_##TAG(ID3D11DeviceContext *ctx, const D3D11StageState<Shader> &s)                                                       \
		{                                                                                                                                           \
			ctx->TAG##SetShader(s.shader, s.instances, s.instance_count);                                                                          \
			ctx->TAG##SetConstantBuffers(0, D3D11StageState<Shader>::kCb, s.cb);                                                                    \
			ctx->TAG##SetShaderResources(0, D3D11StageState<Shader>::kSrv, s.srv);                                                                  \
			ctx->TAG##SetSamplers(0, D3D11StageState<Shader>::kSampler, s.sampler);                                                                 \
		}
		ASTRA_D3D11_STAGE(VS, ID3D11VertexShader)
		ASTRA_D3D11_STAGE(HS, ID3D11HullShader)
		ASTRA_D3D11_STAGE(DS, ID3D11DomainShader)
		ASTRA_D3D11_STAGE(GS, ID3D11GeometryShader)
		ASTRA_D3D11_STAGE(PS, ID3D11PixelShader)
#undef ASTRA_D3D11_STAGE

		/// IA, every shader stage, RS, OM, CS's UAVs - what draw_before captures once before running the effects and
		/// puts back once after.
		struct D3D11Snapshot
		{
			static constexpr UINT kVb = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
			static constexpr UINT kViewport = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			static constexpr UINT kRtv = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
			// OM UAVs: the base interface's shared RTV/UAV register count (8). CS UAVs: as many as the device's feature
			// level has (64 at 11_1, 8 at 11_0, 1 below) - the same counts ReShade's own D3D11 state block saves.
			static constexpr UINT kUav = D3D11_PS_CS_UAV_REGISTER_COUNT;
			static constexpr UINT kCsUav = D3D11_1_UAV_SLOT_COUNT;
			static constexpr UINT kCb = D3D11StageState<ID3D11VertexShader>::kCb;
			static constexpr UINT kSrv = D3D11StageState<ID3D11VertexShader>::kSrv;
			static constexpr UINT kSampler = D3D11StageState<ID3D11VertexShader>::kSampler;
			static constexpr UINT kClass = D3D11StageState<ID3D11VertexShader>::kClass;

			ID3D11InputLayout *input_layout = nullptr;
			D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
			ID3D11Buffer *vb[kVb] = {};
			UINT vb_stride[kVb] = {}, vb_offset[kVb] = {};
			ID3D11Buffer *ib = nullptr;
			DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
			UINT ib_offset = 0;

			D3D11StageState<ID3D11VertexShader> vs;
			D3D11StageState<ID3D11HullShader> hs;
			D3D11StageState<ID3D11DomainShader> ds;
			D3D11StageState<ID3D11GeometryShader> gs;
			D3D11StageState<ID3D11PixelShader> ps;

			ID3D11ComputeShader *cs_shader = nullptr;
			ID3D11ClassInstance *cs_instances[kClass] = {};
			UINT cs_instance_count = kClass;
			ID3D11Buffer *cs_cb[kCb] = {};
			ID3D11ShaderResourceView *cs_srv[kSrv] = {};
			ID3D11SamplerState *cs_sampler[kSampler] = {};
			ID3D11UnorderedAccessView *cs_uav[kCsUav] = {};
			UINT cs_uav_count = kUav; // what this device's feature level binds (d3d11_capture)

			ID3D11RasterizerState *rs_state = nullptr;
			UINT viewport_count = kViewport;
			D3D11_VIEWPORT viewports[kViewport] = {};
			UINT scissor_count = kViewport;
			D3D11_RECT scissors[kViewport] = {};

			ID3D11RenderTargetView *rtv[kRtv] = {};
			ID3D11DepthStencilView *dsv = nullptr;
			ID3D11UnorderedAccessView *om_uav[kUav] = {};
			ID3D11BlendState *blend_state = nullptr;
			FLOAT blend_factor[4] = {};
			UINT sample_mask = 0;
			ID3D11DepthStencilState *depth_state = nullptr;
			UINT stencil_ref = 0;

			D3D11Snapshot() = default;
			D3D11Snapshot(const D3D11Snapshot &) = delete;
			D3D11Snapshot &operator=(const D3D11Snapshot &) = delete;
			~D3D11Snapshot()
			{
				if (input_layout != nullptr)
					input_layout->Release();
				for (ID3D11Buffer *p : vb)
					if (p != nullptr)
						p->Release();
				if (ib != nullptr)
					ib->Release();
				if (cs_shader != nullptr)
					cs_shader->Release();
				for (ID3D11ClassInstance *p : cs_instances)
					if (p != nullptr)
						p->Release();
				for (ID3D11Buffer *p : cs_cb)
					if (p != nullptr)
						p->Release();
				for (ID3D11ShaderResourceView *p : cs_srv)
					if (p != nullptr)
						p->Release();
				for (ID3D11SamplerState *p : cs_sampler)
					if (p != nullptr)
						p->Release();
				for (ID3D11UnorderedAccessView *p : cs_uav)
					if (p != nullptr)
						p->Release();
				if (rs_state != nullptr)
					rs_state->Release();
				for (ID3D11RenderTargetView *p : rtv)
					if (p != nullptr)
						p->Release();
				if (dsv != nullptr)
					dsv->Release();
				for (ID3D11UnorderedAccessView *p : om_uav)
					if (p != nullptr)
						p->Release();
				if (blend_state != nullptr)
					blend_state->Release();
				if (depth_state != nullptr)
					depth_state->Release();
			}
		};

		void d3d11_capture(ID3D11DeviceContext *ctx, D3D11Snapshot &s)
		{
			ctx->IAGetInputLayout(&s.input_layout);
			ctx->IAGetPrimitiveTopology(&s.topology);
			ctx->IAGetVertexBuffers(0, D3D11Snapshot::kVb, s.vb, s.vb_stride, s.vb_offset);
			ctx->IAGetIndexBuffer(&s.ib, &s.ib_format, &s.ib_offset);
			d3d11_capture_VS(ctx, s.vs);
			d3d11_capture_HS(ctx, s.hs);
			d3d11_capture_DS(ctx, s.ds);
			d3d11_capture_GS(ctx, s.gs);
			d3d11_capture_PS(ctx, s.ps);
			s.cs_instance_count = D3D11Snapshot::kClass;
			ctx->CSGetShader(&s.cs_shader, s.cs_instances, &s.cs_instance_count);
			ctx->CSGetConstantBuffers(0, D3D11Snapshot::kCb, s.cs_cb);
			ctx->CSGetShaderResources(0, D3D11Snapshot::kSrv, s.cs_srv);
			ctx->CSGetSamplers(0, D3D11Snapshot::kSampler, s.cs_sampler);
			{
				ID3D11Device *dev11 = nullptr;
				ctx->GetDevice(&dev11);
				const D3D_FEATURE_LEVEL fl = dev11 != nullptr ? dev11->GetFeatureLevel() : D3D_FEATURE_LEVEL_11_0;
				if (dev11 != nullptr)
					dev11->Release();
				s.cs_uav_count = fl >= D3D_FEATURE_LEVEL_11_1 ? D3D11Snapshot::kCsUav
					: fl == D3D_FEATURE_LEVEL_11_0				 ? D3D11_PS_CS_UAV_REGISTER_COUNT
																 : D3D11_CS_4_X_UAV_REGISTER_COUNT;
			}
			ctx->CSGetUnorderedAccessViews(0, s.cs_uav_count, s.cs_uav);
			ctx->RSGetState(&s.rs_state);
			s.viewport_count = D3D11Snapshot::kViewport;
			ctx->RSGetViewports(&s.viewport_count, s.viewports);
			s.scissor_count = D3D11Snapshot::kViewport;
			ctx->RSGetScissorRects(&s.scissor_count, s.scissors);
			// two calls, not one: RTV/DSV and UAVs share the OM stage's 8 slots (UAVStartSlot must equal the real
			// RTV count, which this capture does not know ahead of time) - NumRTVs=0 / NumUAVs=0 is the documented
			// way to read only the other half without that overlap.
			ctx->OMGetRenderTargetsAndUnorderedAccessViews(D3D11Snapshot::kRtv, s.rtv, &s.dsv, 0, 0, nullptr);
			ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11Snapshot::kUav, s.om_uav);
			ctx->OMGetBlendState(&s.blend_state, s.blend_factor, &s.sample_mask);
			ctx->OMGetDepthStencilState(&s.depth_state, &s.stencil_ref);
		}

		void d3d11_restore(ID3D11DeviceContext *ctx, const D3D11Snapshot &s)
		{
			ctx->IASetInputLayout(s.input_layout);
			ctx->IASetPrimitiveTopology(s.topology);
			ctx->IASetVertexBuffers(0, D3D11Snapshot::kVb, s.vb, s.vb_stride, s.vb_offset);
			ctx->IASetIndexBuffer(s.ib, s.ib_format, s.ib_offset);
			d3d11_restore_VS(ctx, s.vs);
			d3d11_restore_HS(ctx, s.hs);
			d3d11_restore_DS(ctx, s.ds);
			d3d11_restore_GS(ctx, s.gs);
			d3d11_restore_PS(ctx, s.ps);
			ctx->CSSetShader(s.cs_shader, s.cs_instances, s.cs_instance_count);
			ctx->CSSetConstantBuffers(0, D3D11Snapshot::kCb, s.cs_cb);
			ctx->CSSetShaderResources(0, D3D11Snapshot::kSrv, s.cs_srv);
			ctx->CSSetSamplers(0, D3D11Snapshot::kSampler, s.cs_sampler);
			UINT keep_counts[D3D11Snapshot::kCsUav];
			std::fill(std::begin(keep_counts), std::end(keep_counts), UINT(-1)); // append/consume counters kept as they are
			ctx->CSSetUnorderedAccessViews(0, s.cs_uav_count, s.cs_uav, keep_counts);
			ctx->RSSetState(s.rs_state);
			ctx->RSSetViewports(s.viewport_count, s.viewports);
			ctx->RSSetScissorRects(s.scissor_count, s.scissors);
			// RTV/DSV as ReShade's own state block puts them back. The OM UAVs (none in a tonemapper, and no ReShade
			// effect binds one) only if the game had any: from the first bound one - always past the bound RTVs, since
			// the capture read slots an RTV holds as empty - so the call can never overlap (and unbind) a render target.
			ctx->OMSetRenderTargets(D3D11Snapshot::kRtv, s.rtv, s.dsv);
			UINT first_uav = D3D11Snapshot::kUav;
			for (UINT i = 0; i < D3D11Snapshot::kUav && first_uav == D3D11Snapshot::kUav; ++i)
				if (s.om_uav[i] != nullptr)
					first_uav = i;
			if (first_uav < D3D11Snapshot::kUav)
				ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, first_uav,
					D3D11Snapshot::kUav - first_uav, s.om_uav + first_uav, keep_counts);
			ctx->OMSetBlendState(s.blend_state, s.blend_factor, s.sample_mask);
			ctx->OMSetDepthStencilState(s.depth_state, s.stencil_ref);
		}

		/// Holds g_recording for one pass (released when it goes out of scope).
		struct Recording
		{
			bool held = false;
			Recording()
			{
				bool idle = false;
				if (g_presenting.load() || !g_recording.compare_exchange_strong(idle, true))
					return;
				if (g_presenting.load()) // Present began between the two: it waits for us, but let it go first
				{
					g_recording.store(false);
					return;
				}
				held = true;
			}
			~Recording()
			{
				if (held)
					g_recording.store(false);
			}
		};

		/// The tonemapper is about to draw: her pass into its input (a copy of it, copied back).
		void draw_before(command_list *cmd, ListState &s, const passfind::Tex &scene, uint32_t scene_state)
		{
			++g_found;
			const uint64_t frame = g_presents.load();
			if (g_drawnAt.load() == frame || !on())
				return; // tried this frame already (a second view, a scene capture); pretone off
			device *dev = cmd->get_device();
			effect_runtime *runtime = g_runtime.load();
			if (runtime == nullptr || runtime->get_device() != dev)
				return;
			const device_api api = dev->get_api();
			if (api != device_api::d3d11 && api != device_api::d3d12)
			{
				if (!g_loggedApi)
				{
					g_loggedApi = true;
					logf("pretone: a tonemapper pass, but not Direct3D 11 or 12 (%s): she is drawn at presentation", api_name(api));
				}
				return;
			}
			// D3D12 only: a list not tracked whole since pretone turned on - what it bound before that is unknown,
			// and restore() would put back an incomplete root signature. D3D11 has no such gap: its state is
			// sticky and read straight off the context below, never reconstructed from what this add-on tracked.
			if (api == device_api::d3d12 && s.epoch != g_onEpoch.load())
				return;
			ID3D11DeviceContext *ctx11 = nullptr;
			if (api == device_api::d3d11)
			{
				ctx11 = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
				if (ctx11->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED)
				{
					++g_deferred; // phase A: only the immediate context draws; stay at Present for this list
					if (!g_loggedDeferred)
					{
						g_loggedDeferred = true;
						logf("pretone: the tonemapper pass is on a deferred Direct3D 11 context: not drawn there (not yet supported); she is drawn "
							 "at presentation for its frames");
					}
					return;
				}
			}
			const Recording rec;
			if (!rec.held)
			{
				++g_contended; // Present is in ReShade now, or another thread draws her pass: at Present this frame
				return;
			}
			if (g_drawnAt.load() == frame || !pretone_rules::should_try(g_fallback, GetTickCount64(), runtime->get_effects_state()))
				return;
			// g_stability (like g_fallback above) is touched only here, past the Recording guard: one thread at a time.
			{
				const bool gave_up_before = g_stability.given_up;
				const bool ready = g_stability.ready(s.find.vp_w, s.find.vp_h, g_outW, g_outH);
				if (!gave_up_before && g_stability.given_up && !g_loggedResChanges)
				{
					g_loggedResChanges = true;
					logf("pretone: the game's render resolution keeps changing (dynamic resolution): she is drawn at presentation for the rest of "
						 "this session");
				}
				if (!ready)
					return; // this size has not held for pretone_rules::Stability::kStable frames yet (or gave up): at Present
			}
			g_beginFrame(); // this is the frame being drawn: its camera now (its picture is waited for in the effect)
			if (!layer::active() || !layer::tone_ready())
				return; // she is not out (the game's picture passes through at Present), or no Present ran the effects yet
			const resource game = {scene.id};
			// her copy (and the effects' target) is the tonemapper's IMAGE - its viewport - never the padded target it
			// may sit inside (Unreal's pool): clamped defensively to the target's own size (the exact-size case tolerates
			// the viewport overshooting it by 1 px, a float-rounding slop; the copy and the box below never do)
			const uint32_t vp_w = s.find.vp_w < scene.w ? s.find.vp_w : scene.w, vp_h = s.find.vp_h < scene.h ? s.find.vp_h : scene.h;
			std::lock_guard<std::mutex> lock(g_targetLock);
			if (!ensure_copy(dev, game, vp_w, vp_h, frame))
			{
				if (!g_loggedFail)
				{
					g_loggedFail = true;
					logf("pretone: cannot make a copy of the game's scene colour (%ux%u): she is drawn at presentation", vp_w, vp_h);
				}
				return;
			}
			if (!g_loggedFound)
			{
				g_loggedFound = true;
				const resource_desc d = dev->get_resource_desc(game);
				logf("pretone: the game's tonemapper found (%s): it reads a %ux%u scene colour (format %u), its image %ux%u of the presented "
					 "%ux%u; she is drawn into it, before the game's tone curve, grade, bloom and UI",
					api_name(api), scene.w, scene.h, uint32_t(d.texture.format), vp_w, vp_h, g_outW.load(), g_outH.load());
			}
			g_api = uint32_t(api);
			const resource_usage was = resource_usage(scene_state);
			if (api == device_api::d3d12) // D3D11 never fires barrier events (no transitions there) - no-op
			{
				const resource r[2] = {game, g_copy};
				const resource_usage from[2] = {was, resource_usage::shader_resource}, to[2] = {resource_usage::copy_source, resource_usage::copy_dest};
				cmd->barrier(2, r, from, to);
			}
			// the box is the viewport's own corner of the padded target - what copy_texture_region moves either way
			const subresource_box box{0, 0, 0, vp_w, vp_h, 1};
			cmd->copy_texture_region(game, 0, &box, g_copy, 0, nullptr);
			if (api == device_api::d3d12)
				cmd->barrier(g_copy, resource_usage::copy_dest, resource_usage::render_target);
			// D3D11: ReShade's render_effects saves and restores none of the context's state - capture it here,
			// right before, and put it back right after (D3D12's restore(), by contrast, is built from what this
			// add-on tracked, since D3D12 has nothing like these Get* calls to read the current state back from).
			D3D11Snapshot snap11;
			if (api == device_api::d3d11)
				d3d11_capture(ctx11, snap11);
			const unsigned long long runs = layer::her_runs();
			layer::set_hdr_pass(true, frame, vp_w, vp_h);
			runtime->render_effects(cmd, g_copyRtv, g_copyRtv);
			layer::set_hdr_pass(false, frame);
			// her technique drew - not: the effects compile for this target or reload. ReShade has marked this frame's
			// effects drawn either way, so nothing is drawn at Present this frame: one lost frame, then the Present
			// path is held a while (pretone_rules::Fallback)
			const bool ran = layer::her_runs() != runs;
			if (api == device_api::d3d12)
			{
				const resource r[2] = {g_copy, game};
				const resource_usage from[2] = {resource_usage::render_target, resource_usage::copy_source},
									 to[2] = {resource_usage::copy_source, ran ? resource_usage::copy_dest : was};
				cmd->barrier(2, r, from, to);
			}
			if (ran)
			{
				cmd->copy_texture_region(g_copy, 0, nullptr, game, 0, &box);
				if (api == device_api::d3d12)
					cmd->barrier(game, resource_usage::copy_dest, was);
			}
			if (api == device_api::d3d12)
			{
				cmd->barrier(g_copy, resource_usage::copy_source, resource_usage::shader_resource);
				restore(cmd, s);
			}
			else
			{
				d3d11_restore(ctx11, snap11);
			}
			g_drawnAt = frame;
			if (ran)
			{
				g_ranAt = frame;
				++g_before;
				++g_periodBefore;
			}
			if (g_fallback.result(GetTickCount64(), ran))
			{
				if (!g_loggedHold)
				{
					g_loggedHold = true;
					logf("pretone: her pass before the tonemapper did not draw (the effects are compiling for its %ux%u scene colour): one frame "
						 "without effects; she is drawn at presentation for %.1f s, then tried again",
						scene.w, scene.h, double(g_fallback.hold) / 1000.0);
				}
				if (g_fallback.given_up && !g_loggedGiveUp)
				{
					g_loggedGiveUp = true;
					logf("pretone: her pass before the tonemapper did not draw %d times in a row: she is drawn at presentation from now on",
						g_fallback.misses);
				}
			}
		}

		/// ASTRA_PRETONE_TRACE: one line per candidate pass - what it binds, and the verdict.
		void trace_pass(command_list *cmd, const char *what, const passfind::Tex *bound, size_t n, const char *verdict)
		{
			char list[480] = "";
			size_t used = 0;
			for (size_t i = 0; i < n && used + 48 < sizeof(list); ++i)
				used += size_t(std::snprintf(list + used, sizeof(list) - used, " %llx:%u:%ux%u%s", (unsigned long long)bound[i].id,
					format_of(cmd->get_device(), bound[i].id), bound[i].w, bound[i].h, bound[i].written ? "W" : ""));
			++g_traceLines;
			logf("trace %lu %p: %s binding [%s ] -> %s", GetCurrentThreadId(), (void *)cmd, what, list, verdict);
		}

		/// This draw's descriptor copies: the ones since the list's previous draw (and the mark moves on).
		uint32_t take_window(ListState &s)
		{
			const uint32_t w = pretone_rules::window(t_ring, s.mark);
			s.mark = pretone_rules::Mark{&t_ring, t_ring.next};
			return w;
		}

		bool on_draw_any(command_list *cmd, uint32_t vertices, uint32_t instances)
		{
			if (!on())
				return false;
			ListState *s = state(cmd);
			if (s == nullptr)
				return false;
			device *dev = cmd->get_device();
			const bool d3d11 = dev->get_api() == device_api::d3d11;
			const uint32_t window = take_window(*s); // D3D12 only; a no-op bookkeeping call on every other API
			const bool shape = passfind::tonemapper_shape(s->find, vertices, instances, g_outW, g_outH);
			const bool trace = tracing() && vertices <= 6 && s->find.target.id != 0 &&
							   passfind::trace_candidate_size(s->find.target.w, s->find.target.h, g_outW, g_outH);
			if (!shape && !trace)
				return false;
			if (shape && !trace && g_foundAt.load() == g_presents.load())
				return false; // the tonemapper was this frame's already (the UI's quads come after it)
			passfind::Tex reads[24];
			const size_t n = d3d11 ? d3d11_reads(cmd, false, reads, 24) : draw_reads(cmd, s->gfx, window, reads, 24);
			bool ambiguous = false;
			const passfind::Tex scene = shape ? passfind::scene_read(s->find, reads, n, &ambiguous) : passfind::Tex();
			if (trace)
			{
				char what[200];
				std::snprintf(what, sizeof(what), "%s draw %u x%u into %llx (format %u %ux%u, %u targets, viewport %ux%u, presented=%d near=%d)",
					api_name(dev->get_api()), vertices, instances, (unsigned long long)s->find.target.id, format_of(dev, s->find.target.id),
					s->find.target.w, s->find.target.h, s->find.targets_bound, s->find.vp_w, s->find.vp_h,
					passfind::presented_size(s->find.target.w, s->find.target.h, g_outW, g_outH) ? 1 : 0,
					passfind::near_presented_size(s->find.target.w, s->find.target.h, g_outW, g_outH) ? 1 : 0);
				trace_pass(cmd, what, reads, n, scene.id != 0 ? "THE TONEMAPPER" : shape ? "full-screen, no HDR input" : "-");
			}
			if (scene.id == 0)
				return false;
			// below the presented size, the shape alone would also take a scene capture's own tonemap (an in-game
			// camera, a monitor, at the view's aspect): only the MAIN view's is drawn into - the one whose scene colour
			// is on the grid of the depth buffer ReShade's depth add-on picked (Unreal sizes both from one extent).
			// Checked BEFORE the frame's tonemapper is claimed, so a capture recorded first never hides the real one.
			if (!passfind::presented_size(s->find.target.w, s->find.target.h, g_outW, g_outH))
			{
				uint32_t dw = 0, dh = 0;
				const bool have_depth = layer::depth_size(dw, dh);
				if (!passfind::main_view_grid(scene, have_depth ? dw : 0, have_depth ? dh : 0))
				{
					if (!g_loggedNotMain)
					{
						g_loggedNotMain = true;
						logf("pretone: a tonemapper below the presented size reads a %ux%u scene colour, but the depth buffer is %ux%u: "
							 "not the main view (a scene capture?) - not drawn into",
							scene.w, scene.h, dw, dh);
					}
					return false;
				}
			}
			if (g_foundAt.exchange(g_presents.load()) == g_presents.load())
				return false;
			if (ambiguous && !g_loggedAmbiguous)
			{
				g_loggedAmbiguous = true;
				logf("pretone: the tonemapper reads more than one HDR picture of its size: the first one bound is taken");
			}
			draw_before(cmd, *s, scene, uint32_t(read_state(scene.id, true)));
			return false; // the game's draw always runs
		}

		/// A compute tonemapper: a dispatch that writes the presented size's 8/10-bit picture from its HDR one.
		bool on_dispatch(command_list *cmd, uint32_t, uint32_t, uint32_t)
		{
			if (!on())
				return false;
			ListState *s = state(cmd);
			if (s == nullptr)
				return false;
			device *dev = cmd->get_device();
			const bool d3d11 = dev->get_api() == device_api::d3d11;
			const uint32_t window = take_window(*s);
			if (!tracing() || (!d3d11 && s->cmp.layout.handle == 0))
				return false; // only looked at in a trace: a game's frame has hundreds of dispatches
			passfind::Tex bound[24];
			const size_t n = d3d11 ? d3d11_reads(cmd, true, bound, 24) : draw_reads(cmd, s->cmp, window, bound, 24);
			bool ambiguous = false, sized = false;
			const passfind::Tex scene = passfind::compute_scene(bound, n, g_outW, g_outH, &ambiguous);
			for (size_t i = 0; i < n; ++i)
				sized = sized || passfind::trace_candidate_size(bound[i].w, bound[i].h, g_outW, g_outH);
			if (sized)
			{
				char what[32];
				std::snprintf(what, sizeof(what), "%s dispatch", api_name(dev->get_api()));
				trace_pass(cmd, what, bound, n, scene.id != 0 ? "a compute tonemapper (not drawn into)" : "-");
			}
			// recognised (in a trace), not drawn into: no game with one has been run, and a pass taken for it wrongly
			// breaks the game's GPU state - she is drawn at presentation unless a tonemapper draw is found
			if (scene.id != 0 && !g_loggedCompute)
			{
				g_loggedCompute = true;
				logf("pretone: a compute pass looks like the tonemapper (it writes a %ux%u 8/10-bit picture from an HDR one%s): not drawn there "
					 "(untested); she is drawn at presentation unless a tonemapper draw is found",
					scene.w, scene.h, ambiguous ? ", one of several" : "");
			}
			return false; // the game's dispatch always runs
		}

		bool on_draw(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t)
		{
			return on_draw_any(cmd, vertices, instances);
		}

		bool on_draw_indexed(command_list *cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t)
		{
			return on_draw_any(cmd, indices, instances);
		}

		/// An indirect draw or dispatch is never the tonemapper, but its descriptors were copied before it like any.
		bool on_draw_or_dispatch_indirect(command_list *cmd, indirect_command, resource, uint64_t, uint32_t, uint32_t)
		{
			if (!on())
				return false;
			if (ListState *s = state(cmd))
				take_window(*s);
			return false;
		}

		void on_init_effect_runtime(effect_runtime *runtime)
		{
			g_runtime = runtime;
		}

		void on_destroy_effect_runtime(effect_runtime *runtime)
		{
			effect_runtime *expected = runtime;
			g_runtime.compare_exchange_strong(expected, nullptr);
			std::lock_guard<std::mutex> lock(g_targetLock);
			if (g_device == runtime->get_device())
			{
				runtime->get_command_queue()->wait_idle(); // nothing in flight reads a copy any more
				retire_copy(0);
				g_retired.collect(~uint64_t(0) >> 1, destroy_now);
				g_device = nullptr;
			}
		}

		/// After ReShade's Present work (its effects at Present, its overlay): recording threads may draw her pass again.
		void on_reshade_present(effect_runtime *)
		{
			g_presenting.store(false);
		}

		/// Where she is drawn, decided again each Present (pretone_rules::choose); a change is logged.
		void decide()
		{
			pretone_rules::Facts f;
			f.path_present = g_path != 0;
			f.declared = provider::state().engine();
			f.ini = g_iniEngine;
			f.hdr_output = g_hdrOutput;
			f.present_callback = provider::state().present_callback().fn != nullptr;
			const pretone_rules::Choice c = pretone_rules::choose(f);
			const bool before = c.where == pretone_rules::Where::before;
			if (before != g_on.load() || c.why != g_why)
			{
				if (before && !g_on.load())
					++g_onEpoch; // lists reset from now on are tracked whole
				g_on.store(before);
				layer::set_tone_wanted(before);
				g_why = c.why;
				logf("pretone: %s: %s%s", before ? "her pass goes before the game's tonemapper when it is found" : "she is drawn at presentation", c.why,
					before ? (f.declared.empty() ? " ([tone] Engine= of the launcher's ini)" : " (declared by the provider)") : "");
			}
		}
	}

	void install(void (*begin_frame)(), const std::string &ini_engine)
	{
		g_beginFrame = begin_frame;
		g_iniEngine = ini_engine;
		char trace[32] = "";
		if (GetEnvironmentVariableA("ASTRA_PRETONE_TRACE", trace, sizeof(trace)) > 0)
			g_traceFrame = std::strtoull(trace, nullptr, 10);
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
		reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
		reshade::register_event<reshade::addon_event::end_render_pass>(on_end_render_pass);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::bind_pipeline_states>(on_bind_pipeline_states);
		reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
		reshade::register_event<reshade::addon_event::bind_scissor_rects>(on_bind_scissor_rects);
		reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
		reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
		reshade::register_event<reshade::addon_event::push_constants>(on_push_constants);
		reshade::register_event<reshade::addon_event::barrier>(on_barrier);
		reshade::register_event<reshade::addon_event::copy_descriptor_tables>(on_copy_descriptor_tables);
		reshade::register_event<reshade::addon_event::init_pipeline_layout>(on_init_pipeline_layout);
		reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(on_destroy_pipeline_layout);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_or_dispatch_indirect);
		reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
	}

	void present_begins()
	{
		g_presenting.store(true);
		while (g_recording.load()) // a pass on a recording thread: let it finish before ReShade's Present work
			SwitchToThread();
		layer::set_hdr_pass(false, g_presents.load()); // the staging ring's clock for the effects at Present (layer.cpp commit)
	}

	void presented(swapchain *swap, uint32_t width, uint32_t height)
	{
		g_outW = width;
		g_outH = height;
		const uint64_t frame = g_presents.load();
		const bool tried = g_drawnAt.load() == frame, ran = g_ranAt.load() == frame;
		if (!tried && layer::active())
		{
			++g_atPresent;
			++g_periodPresent;
		}
		if (tried && !ran)
		{
			++g_lost;
			++g_periodLost;
		}
		if (swap != nullptr)
		{
			const color_space cs = swap->get_color_space();
			const bool hdr = cs != color_space::unknown && cs != color_space::srgb;
			if (hdr != g_hdrOutput)
			{
				g_hdrOutput = hdr;
				if (hdr && !g_loggedHdr)
				{
					g_loggedHdr = true;
					logf("pretone: the game presents HDR (colour space %u): she is drawn at presentation, her colours not mapped to HDR", uint32_t(cs));
				}
			}
		}
		decide();
		{
			std::lock_guard<std::mutex> lock(g_targetLock);
			g_retired.collect(frame, destroy_now);
		}
		const ULONGLONG now = GetTickCount64();
		if (g_nextStats == 0)
			g_nextStats = now + 30000;
		if (now >= g_nextStats)
		{
			g_nextStats = now + 30000;
			const uint64_t b = g_periodBefore.exchange(0), p = g_periodPresent.exchange(0), l = g_periodLost.exchange(0), c = g_contended.exchange(0);
			if (b + p + l > 0)
				logf("pretone: last 30 s: %llu frames drawn before the tonemapper, %llu at presentation, %llu without effects (compiling); %llu "
					 "tonemapper passes met Present in progress",
					(unsigned long long)b, (unsigned long long)p, (unsigned long long)l, (unsigned long long)c);
			if (g_found == 0 && !g_loggedNone && g_on.load())
			{
				g_loggedNone = true;
				logf("pretone: no tonemapper pass found in 30 s: she is drawn at presentation (over the game's UI)");
			}
		}
		if (!g_on.load())
			std::snprintf(g_status, sizeof(g_status), "drawn at presentation (over the game's UI): %s", g_why);
		else if (g_before > 0)
			std::snprintf(g_status, sizeof(g_status),
				"drawn before the game's tonemapper (%s; its grade, bloom and UI apply): %llu frames, %llu at presentation, %llu without effects",
				api_name(device_api(g_api.load())), (unsigned long long)g_before.load(), (unsigned long long)g_atPresent.load(),
				(unsigned long long)g_lost.load());
		else
			std::snprintf(g_status, sizeof(g_status), "drawn at presentation (over the game's UI): %s",
				g_found > 0 ? "a tonemapper is found, her pass is not drawn there yet" : "no tonemapper pass found yet");
		g_presents = frame + 1;
	}

	int &path()
	{
		return g_path;
	}

	const char *status()
	{
		return g_status;
	}
}
