#include "pretone.h"
#include "layer.h"
#include "log.h"
#include "passfind.h"
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
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
		std::atomic<uint64_t> g_presents{0}, g_drawnAt{~uint64_t(0)}, g_foundAt{~uint64_t(0)};
		int g_path = 0;

		// her pass's own target: a copy of the game's scene colour (same size and format)
		std::mutex g_targetLock;
		device *g_device = nullptr;
		resource g_copy = {0};
		resource_view g_copyRtv = {0};
		resource_desc g_copyDesc;

		// what happened, for the log and the overlay
		std::atomic<uint64_t> g_before{0}, g_atPresent{0}, g_found{0}, g_periodBefore{0}, g_periodPresent{0};
		ULONGLONG g_nextStats = 0;
		bool g_loggedFound = false, g_loggedAmbiguous = false, g_loggedCompute = false, g_loggedHdr = false, g_loggedNone = false, g_loggedApi = false, g_loggedFail = false;
		char g_status[220] = "drawn at presentation (over the game's UI): no frame yet";

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

		void on_init_command_list(command_list *cmd)
		{
			cmd->create_private_data<ListState>();
		}

		void on_destroy_command_list(command_list *cmd)
		{
			cmd->destroy_private_data<ListState>();
		}

		void on_reset_command_list(command_list *cmd)
		{
			if (ListState *s = state(cmd))
				s->clear();
		}

		void on_bind_render_targets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
		{
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
			if (ListState *s = state(cmd))
				s->find.in_pass = true;
			return false;
		}

		bool on_end_render_pass(command_list *cmd)
		{
			if (ListState *s = state(cmd))
				s->find.in_pass = false;
			return false;
		}

		void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline p)
		{
			ListState *s = state(cmd);
			if (s != nullptr && (stages & (pipeline_stage::all_graphics | pipeline_stage::all_compute)) != 0)
			{
				s->pso = p; // Direct3D 12 has one pipeline state slot: the last one bound, graphics or compute
				s->pso_stages = stages;
			}
		}

		void on_bind_pipeline_states(command_list *cmd, uint32_t count, const dynamic_state *states, const uint32_t *values)
		{
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
			ListState *s = state(cmd);
			if (s == nullptr)
				return;
			if (s->viewports.size() < first + count)
				s->viewports.resize(first + count);
			for (uint32_t i = 0; i < count; ++i)
				s->viewports[first + i] = vps[i];
			if (first == 0 && count > 0)
				s->find.viewport(vps[0].width, vps[0].height);
		}

		void on_bind_scissor_rects(command_list *cmd, uint32_t first, uint32_t count, const rect *rects)
		{
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

		void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t index, const descriptor_table_update &update)
		{
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
			constexpr resource_usage writes = resource_usage::render_target | resource_usage::unordered_access | resource_usage::depth_stencil_write |
											  resource_usage::copy_dest | resource_usage::vertex_buffer;
			for (uint32_t i = 0; i < count; ++i)
			{
				if (resources[i].handle == 0 || (new_states[i] & resource_usage::shader_resource) == 0 || (new_states[i] & writes) != 0)
					continue;
				const passfind::Tex t = describe(cmd->get_device(), resources[i]);
				if (t.kind != passfind::Kind::hdr || !passfind::presented_size(t.w, t.h, g_outW, g_outH))
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
		// right before it, on the thread recording it (Unreal's descriptor cache does). A ring of this thread's recent
		// copies maps a bound table's slots back to the views copied there, and the views to their textures.
		struct Copy
		{
			uint64_t heap;     // the destination heap (the original ID3D12DescriptorHeap, as ReShade hands it out)
			uint32_t off;      // the first destination slot
			uint32_t count;
			uint64_t src_heap; // where the first source descriptor is: ReShade names a view by its ORIGINAL CPU handle
			uint32_t src_off;
		};
		// copies kept per thread; how far back a draw looks (its own descriptors were copied right before it); slots of a
		// table looked at
		constexpr uint32_t kRing = 1024, kLookBack = 128, kSpan = 32;
		struct Ring
		{
			Copy c[kRing];
			uint32_t next;
		};
		thread_local Ring t_ring;

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
			if (dev->get_api() != device_api::d3d12)
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
				Copy &slot = t_ring.c[t_ring.next++ % kRing];
				slot.heap = heap.handle;
				slot.off = off;
				slot.count = c.count;
				slot.src_heap = src_heap.handle;
				slot.src_off = src_off;
			}
			return false;
		}

		/// The textures the draw about to be recorded reads, through the tables bound for it (newest copy wins a slot).
		size_t draw_reads(command_list *cmd, const ListState::Root &s, passfind::Tex *out, size_t cap)
		{
			device *dev = cmd->get_device();
			const uint32_t inc = g_increment;
			size_t n = 0;
			if (inc == 0)
				return 0;
			std::vector<std::vector<Range>> ranges; // this root signature's table ranges (a copy: the lock is not held long)
			{
				std::shared_lock<std::shared_mutex> lock(g_layoutLock);
				if (const auto it = g_layouts.find(s.layout.handle); it != g_layouts.end())
					ranges = it->second;
			}
			// where each table starts: a table's slots end where the next one bound from the same heap begins (a game
			// lays the tables of one draw side by side, and declares more slots than it fills)
			constexpr size_t kTables = 16;
			uint64_t heaps[kTables] = {};
			uint32_t bases[kTables] = {};
			const size_t tables = std::min(s.params.size(), kTables);
			for (size_t pi = 0; pi < tables; ++pi)
				if (s.params[pi].kind == ListState::Arg::table && s.params[pi].table.handle != 0)
				{
					descriptor_heap heap = {0};
					dev->get_descriptor_heap_offset(s.params[pi].table, 0, 0, &heap, &bases[pi]);
					heaps[pi] = heap.handle;
				}
			for (size_t pi = 0; pi < tables; ++pi)
			{
				if (heaps[pi] == 0)
					continue;
				const uint32_t base = bases[pi];
				uint32_t end = base + kSpan;
				for (size_t q = 0; q < tables; ++q)
					if (q != pi && heaps[q] == heaps[pi] && bases[q] > base && bases[q] < end)
						end = bases[q];
				uint32_t seen = 0; // slots of this table resolved already (by a newer copy)
				for (uint32_t k = 0; k < kLookBack && k < t_ring.next && seen != ~0u; ++k)
				{
					const Copy &c = t_ring.c[(t_ring.next - 1 - k) % kRing];
					if (c.heap != heaps[pi] || c.count == 0 || c.off >= end || c.off + c.count <= base)
						continue;
					for (uint32_t j = 0; j < c.count; ++j)
					{
						const uint32_t slot = c.off + j;
						if (slot < base || slot >= end || (seen & (1u << (slot - base))) != 0)
							continue;
						seen |= 1u << (slot - base);
						if (c.src_heap == 0)
							continue;
						// the view's original CPU handle: its heap's start + its slot (the heap is the original object)
						const D3D12_CPU_DESCRIPTOR_HANDLE start =
							reinterpret_cast<ID3D12DescriptorHeap *>(c.src_heap)->GetCPUDescriptorHandleForHeapStart();
						const resource r = dev->get_resource_from_view(resource_view{start.ptr + uint64_t(c.src_off + j) * inc});
						passfind::Tex t = describe(dev, r);
						if (t.id == 0)
							continue;
						t.written = slot_written(&ranges, pi, slot - base);
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
				}
			}
			return n;
		}

		void destroy_copy()
		{
			if (g_device != nullptr)
			{
				if (g_copyRtv.handle != 0)
					g_device->destroy_resource_view(g_copyRtv);
				if (g_copy.handle != 0)
					g_device->destroy_resource(g_copy);
			}
			g_copy = {0};
			g_copyRtv = {0};
		}

		/// Her pass's target: like the game's scene colour (its size, its format typed), and writable by ReShade.
		bool ensure_copy(device *dev, resource scene)
		{
			const resource_desc d = dev->get_resource_desc(scene);
			const format typed = format(passfind::typed_hdr(uint32_t(d.texture.format)));
			if (g_copy.handle != 0 && g_device == dev && g_copyDesc.texture.width == d.texture.width && g_copyDesc.texture.height == d.texture.height &&
				g_copyDesc.texture.format == typed)
				return true;
			destroy_copy();
			g_device = dev;
			g_copyDesc = resource_desc(d.texture.width, d.texture.height, 1, 1, typed, 1, memory_heap::default_,
				resource_usage::render_target | resource_usage::shader_resource | resource_usage::copy_source | resource_usage::copy_dest);
			if (!dev->create_resource(g_copyDesc, nullptr, resource_usage::shader_resource, &g_copy))
			{
				g_copy = {0};
				return false;
			}
			dev->set_resource_name(g_copy, "Astra: the game's scene colour, her pass");
			if (!dev->create_resource_view(g_copy, resource_usage::render_target, resource_view_desc(typed), &g_copyRtv))
			{
				destroy_copy();
				return false;
			}
			return true;
		}

		/// The game's graphics state back, as it was bound before her pass (the tonemapper draws right after).
		void restore(command_list *cmd, const ListState &s)
		{
			const ListState::Root &g = s.gfx;
			if (g.layout.handle != 0)
			{
				// no tables: ReShade puts the game's descriptor heaps and this root signature back
				cmd->bind_descriptor_tables(shader_stage::all_graphics, g.layout, 0, 0, nullptr);
				auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
				for (uint32_t i = 0; i < uint32_t(g.params.size()); ++i)
				{
					const ListState::Param &p = g.params[i];
					switch (p.kind)
					{
					case ListState::Arg::table:
						cmd->bind_descriptor_tables(shader_stage::all_graphics, g.layout, i, 1, &p.table);
						break;
					case ListState::Arg::cbv:
						cmd->push_descriptors(shader_stage::all_graphics, g.layout, i, descriptor_table_update{{}, 0, 0, 1, descriptor_type::constant_buffer, &p.cbv});
						break;
					case ListState::Arg::srv: // ReShade's push_descriptors cannot take a raw address back: native
						native->SetGraphicsRootShaderResourceView(i, p.va);
						break;
					case ListState::Arg::uav:
						native->SetGraphicsRootUnorderedAccessView(i, p.va);
						break;
					case ListState::Arg::constants:
						cmd->push_constants(shader_stage::all_graphics, g.layout, i, 0, uint32_t(p.constants.size()), p.constants.data());
						break;
					case ListState::Arg::none:
						break;
					}
				}
			}
			else if (s.cmp.layout.handle != 0)
			{
				// no graphics root signature in this list yet (a compute tonemapper): the game's heaps back through one of
				// its compute tables, bound again as it was - the same root signature, so its other arguments stay (ReShade's
				// effects never touch the compute root)
				for (uint32_t i = 0; i < uint32_t(s.cmp.params.size()); ++i)
					if (s.cmp.params[i].kind == ListState::Arg::table)
					{
						cmd->bind_descriptor_tables(shader_stage::all_compute, s.cmp.layout, i, 1, &s.cmp.params[i].table);
						break;
					}
			}
			if (s.pso.handle != 0)
				cmd->bind_pipeline(s.pso_stages, s.pso); // last: the slot is shared, the game's next draw or dispatch uses it
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

		/// The tonemapper is about to draw: her pass into its input (a copy of it, copied back).
		void draw_before(command_list *cmd, ListState &s, const passfind::Tex &scene, uint32_t scene_state)
		{
			++g_found;
			const uint64_t frame = g_presents.load();
			if (g_drawnAt.load() == frame || g_path != 0 || g_hdrOutput)
				return; // drawn this frame already (a second view, a scene capture), or not wanted here
			device *dev = cmd->get_device();
			effect_runtime *runtime = g_runtime.load();
			if (runtime == nullptr || runtime->get_device() != dev)
				return;
			if (dev->get_api() != device_api::d3d12)
			{
				if (!g_loggedApi)
				{
					g_loggedApi = true;
					logf("pretone: a tonemapper pass, but not Direct3D 12: she is drawn at presentation");
				}
				return;
			}
			g_beginFrame(); // this is the frame being drawn: its camera now (its picture is waited for in the effect)
			if (!layer::active())
				return; // she is not out: nothing to draw, the game's picture passes through at Present
			const resource game = {scene.id};
			std::lock_guard<std::mutex> lock(g_targetLock);
			if (!ensure_copy(dev, game))
			{
				if (!g_loggedFail)
				{
					g_loggedFail = true;
					logf("pretone: cannot make a copy of the game's scene colour (%ux%u): she is drawn at presentation", scene.w, scene.h);
				}
				return;
			}
			if (!g_loggedFound)
			{
				g_loggedFound = true;
				const resource_desc d = dev->get_resource_desc(game);
				logf("pretone: the game's tonemapper found: it reads a %ux%u scene colour (format %u); she is drawn into it, before the game's tone "
					 "curve, grade, bloom and UI",
					scene.w, scene.h, uint32_t(d.texture.format));
			}
			const resource_usage was = resource_usage(scene_state);
			{
				const resource r[2] = {game, g_copy};
				const resource_usage from[2] = {was, resource_usage::shader_resource}, to[2] = {resource_usage::copy_source, resource_usage::copy_dest};
				cmd->barrier(2, r, from, to);
			}
			cmd->copy_texture_region(game, 0, nullptr, g_copy, 0, nullptr);
			cmd->barrier(g_copy, resource_usage::copy_dest, resource_usage::render_target);
			const unsigned long long runs = layer::effect_runs();
			layer::set_hdr_pass(true);
			runtime->render_effects(cmd, g_copyRtv, g_copyRtv);
			layer::set_hdr_pass(false);
			const bool ran = layer::effect_runs() != runs; // not: effects still compiling for this target, or drawn this frame
			{
				const resource r[2] = {g_copy, game};
				const resource_usage from[2] = {resource_usage::render_target, resource_usage::copy_source},
									 to[2] = {resource_usage::copy_source, ran ? resource_usage::copy_dest : was};
				cmd->barrier(2, r, from, to);
			}
			if (ran)
			{
				cmd->copy_texture_region(g_copy, 0, nullptr, game, 0, nullptr);
				cmd->barrier(game, resource_usage::copy_dest, was);
			}
			cmd->barrier(g_copy, resource_usage::copy_source, resource_usage::shader_resource);
			restore(cmd, s);
			g_drawnAt = frame;
			if (ran)
			{
				++g_before;
				++g_periodBefore;
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

		bool on_draw_any(command_list *cmd, uint32_t vertices, uint32_t instances)
		{
			ListState *s = state(cmd);
			if (s == nullptr)
				return false;
			const bool shape = passfind::tonemapper_shape(s->find, vertices, instances, g_outW, g_outH);
			const bool trace = tracing() && vertices <= 6 && s->find.target.id != 0 &&
							   passfind::presented_size(s->find.target.w, s->find.target.h, g_outW, g_outH);
			if (!shape && !trace)
				return false;
			if (shape && !trace && g_foundAt.load() == g_presents.load())
				return false; // the tonemapper was this frame's already (the UI's quads come after it)
			passfind::Tex reads[24];
			const size_t n = draw_reads(cmd, s->gfx, reads, 24);
			bool ambiguous = false;
			const passfind::Tex scene = shape ? passfind::scene_read(s->find, reads, n, &ambiguous) : passfind::Tex();
			if (trace)
			{
				char what[160];
				std::snprintf(what, sizeof(what), "draw %u x%u into %llx (format %u %ux%u, %u targets, viewport %ux%u)", vertices, instances,
					(unsigned long long)s->find.target.id, format_of(cmd->get_device(), s->find.target.id), s->find.target.w, s->find.target.h,
					s->find.targets_bound, s->find.vp_w, s->find.vp_h);
				trace_pass(cmd, what, reads, n, scene.id != 0 ? "THE TONEMAPPER" : shape ? "full-screen, no HDR input" : "-");
			}
			if (scene.id == 0 || g_foundAt.exchange(g_presents.load()) == g_presents.load())
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
			ListState *s = state(cmd);
			if (s == nullptr || s->cmp.layout.handle == 0 || !tracing())
				return false; // only looked at in a trace: a game's frame has hundreds of dispatches
			passfind::Tex bound[24];
			const size_t n = draw_reads(cmd, s->cmp, bound, 24);
			bool ambiguous = false, sized = false;
			const passfind::Tex scene = passfind::compute_scene(bound, n, g_outW, g_outH, &ambiguous);
			for (size_t i = 0; i < n; ++i)
				sized = sized || passfind::presented_size(bound[i].w, bound[i].h, g_outW, g_outH);
			if (tracing() && sized)
				trace_pass(cmd, "dispatch", bound, n, scene.id != 0 ? "a compute tonemapper (not drawn into)" : "-");
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
				destroy_copy();
				g_device = nullptr;
			}
		}
	}

	void install(void (*begin_frame)())
	{
		g_beginFrame = begin_frame;
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
		reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
	}

	void presented(swapchain *swap, uint32_t width, uint32_t height)
	{
		g_outW = width;
		g_outH = height;
		const uint64_t frame = g_presents.load();
		const bool drawn = g_drawnAt.load() == frame;
		if (!drawn && layer::active())
		{
			++g_atPresent;
			++g_periodPresent;
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
		const ULONGLONG now = GetTickCount64();
		if (g_nextStats == 0)
			g_nextStats = now + 30000;
		if (now >= g_nextStats)
		{
			g_nextStats = now + 30000;
			const uint64_t b = g_periodBefore.exchange(0), p = g_periodPresent.exchange(0);
			if (b + p > 0)
				logf("pretone: last 30 s: %llu frames drawn before the tonemapper, %llu at presentation", (unsigned long long)b, (unsigned long long)p);
			if (g_found == 0 && !g_loggedNone && g_path == 0 && !g_hdrOutput)
			{
				g_loggedNone = true;
				logf("pretone: no tonemapper pass found in 30 s: she is drawn at presentation (over the game's UI)");
			}
		}
		if (g_path != 0)
			std::snprintf(g_status, sizeof(g_status), "drawn at presentation (over the game's UI): [tone] Path=present");
		else if (g_hdrOutput)
			std::snprintf(g_status, sizeof(g_status), "drawn at presentation (over the game's UI): the game presents HDR");
		else if (g_before > 0)
			std::snprintf(g_status, sizeof(g_status), "drawn before the game's tonemapper (its grade, bloom and UI apply): %llu frames, %llu at presentation",
				(unsigned long long)g_before.load(), (unsigned long long)g_atPresent.load());
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
