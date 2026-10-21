#pragma once
// passfind: which of a game's draws is its TONEMAPPER - the pass that turns its HDR scene colour into the picture its
// UI is then drawn over - found from the command stream alone, for any engine that does it the usual way (every
// Unreal 4/5 game does): a FULL-SCREEN draw (3, 4 or 6 vertices, one instance, the viewport covering its target) whose
// FIRST target is an 8/10-bit one the size of the presented picture (Unreal 5 binds a second one beside it), that
// READS an HDR (float) texture of that same size - the scene colour. What a draw reads is found by its caller (pretone.cpp: the descriptors the game copied into the
// tables it bound); Unreal 5 records its frame on many threads at once, so no order of events across command lists
// is relied on - only what this one draw binds.
// Portable (no ReShade, no Windows): formats are DXGI numbers (ReShade's api::format uses the same values), and
// tests/core_tests.cpp drives it with recorded draws.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace passfind
{
	enum class Kind : uint8_t
	{
		other, // anything else (depth, single-channel, integer, compressed...)
		ldr,   // 8/10-bit colour: what a tonemapper writes, what the UI is drawn into
		hdr,   // float colour: a scene colour
	};

	/// A DXGI format's kind. Typeless 16/32-bit RGBA counts as HDR (an engine may make its scene colour typeless to
	/// view it two ways); a draw must also READ it as the tonemapper's input to be taken.
	constexpr Kind kind_of(uint32_t dxgi_format)
	{
		switch (dxgi_format)
		{
		case 1:  // R32G32B32A32_TYPELESS
		case 2:  // R32G32B32A32_FLOAT
		case 9:  // R16G16B16A16_TYPELESS
		case 10: // R16G16B16A16_FLOAT
		case 26: // R11G11B10_FLOAT
			return Kind::hdr;
		case 23: // R10G10B10A2_TYPELESS
		case 24: // R10G10B10A2_UNORM
		case 27: // R8G8B8A8_TYPELESS
		case 28: // R8G8B8A8_UNORM
		case 29: // R8G8B8A8_UNORM_SRGB
		case 87: // B8G8R8A8_UNORM
		case 88: // B8G8R8X8_UNORM
		case 90: // B8G8R8A8_TYPELESS
		case 91: // B8G8R8A8_UNORM_SRGB
		case 92: // B8G8R8X8_TYPELESS
		case 93: // B8G8R8X8_UNORM_SRGB
			return Kind::ldr;
		default:
			return Kind::other;
		}
	}

	/// The typed format to view a scene colour of `dxgi_format` with (a typeless one as float).
	constexpr uint32_t typed_hdr(uint32_t dxgi_format)
	{
		return dxgi_format == 9 ? 10 : dxgi_format == 1 ? 2 : dxgi_format;
	}

	struct Tex
	{
		uint64_t id = 0; // the resource (0: none)
		uint32_t w = 0, h = 0;
		Kind kind = Kind::other;
		bool written = false; // bound for writing (an unordered-access view), not only read
	};

	/// One command list's view of the stream, fed by its events in order.
	struct Tracker
	{
		Tex target;                                // the first colour target bound (id 0: none)
		uint32_t targets_bound = 0;                // how many
		uint32_t vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0; // viewport 0

		bool in_pass = false; // inside an API render pass: nothing can be copied there

		void reset()
		{
			*this = Tracker();
		}
		/// Render targets bound; `first` is the first one.
		void targets(uint32_t count, const Tex &first)
		{
			targets_bound = count;
			target = count >= 1 ? first : Tex();
		}
		/// Viewport 0's size, at (0, 0) (every caller but the sub-resolution shape test below).
		void viewport(float w, float h)
		{
			viewport(0.0f, 0.0f, w, h);
		}
		/// Viewport 0's rectangle: a game that tonemaps below the presented size still sits its viewport at the
		/// target's top-left (Unreal does) - a draw whose doesn't is never taken (tonemapper_shape).
		void viewport(float x, float y, float w, float h)
		{
			const auto round = [](float v) { return v > 0.0f ? uint32_t(v + 0.5f) : 0; };
			vp_x = round(x);
			vp_y = round(y);
			vp_w = round(w);
			vp_h = round(h);
		}
	};

	/// The target is exactly the presented picture's size: the only size her pass is drawn at. One size means one copy
	/// of the scene colour and one effect permutation; a size that follows a game's dynamic resolution would make a
	/// new copy (and a new permutation, compiled and never freed) every time it changes.
	inline bool presented_size(uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h)
	{
		return out_w != 0 && out_h != 0 && w == out_w && h == out_h;
	}

	/// ...or the same aspect and at least half of it (a game that tonemaps at its render resolution and upscales
	/// after): only shown in a trace (ASTRA_PRETONE_TRACE), never drawn into - she is drawn at Present in such a game.
	inline bool near_presented_size(uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h)
	{
		if (out_w == 0 || out_h == 0 || w == 0 || h == 0)
			return false;
		if (w == out_w && h == out_h)
			return true;
		const double a = double(w) / double(h), b = double(out_w) / double(out_h);
		return std::abs(a - b) <= 0.01 * b && w * 2 >= out_w && w <= out_w;
	}

	/// TRACE ONLY, a wider net than near_presented_size: the same aspect within 2%, at any size down to 1/4 of the
	/// presented width - never used to decide what gets drawn into (presented_size is the only drawing rule). A game
	/// that renders well below native and upscales after (Poppy Playtime: 768x436 of a 2560x1440 presentation, 0.3x
	/// width) falls outside near_presented_size's half-width floor; this exists so ASTRA_PRETONE_TRACE still shows
	/// such a game's candidates, so phase B can pick a sub-resolution rule from what a live trace actually sees.
	inline bool trace_candidate_size(uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h)
	{
		if (out_w == 0 || out_h == 0 || w == 0 || h == 0)
			return false;
		if (w == out_w && h == out_h)
			return true;
		const double a = double(w) / double(h), b = double(out_w) / double(out_h);
		return std::abs(a - b) <= 0.02 * b && w * 4 >= out_w;
	}

	/// Could this draw be the tonemapper by its SHAPE (a full-screen draw into one 8/10-bit target)? Cheap: asked of
	/// every draw before what it reads is looked up. `vertices`: its vertex (or index) count. Two cases:
	///   - the EXACT-size one (unchanged): the target IS the presented size, and the viewport is within 1 px of it.
	///   - a SUB-RESOLUTION tonemapper (Unreal's screen percentage, TSR/DLSS/FSR, dynamic resolution): the viewport
	///     sits at the target's top-left (0, 0), has the presented aspect within 1%, is at least 1/4 of the presented
	///     width, and fits inside the target - which may be a pool's padding bigger (at most 16 px or 3% per axis,
	///     whichever is larger; Unreal pools its targets, so a 384x217 image sits inside a 384x220 texture). The
	///     target need not be the presented size here - only the viewport's own shape and its fit inside the target
	///     decide it; the HDR texture a draw reads must still be the TARGET's size (scene_read) - the scene colour
	///     shares the padded grid, never the viewport's cropped one.
	inline bool tonemapper_shape(const Tracker &t, uint32_t vertices, uint32_t instances, uint32_t out_w, uint32_t out_h)
	{
		if (t.in_pass || t.target.id == 0 || t.target.kind != Kind::ldr)
			return false;
		if ((vertices != 3 && vertices != 4 && vertices != 6) || instances != 1)
			return false;
		if (t.vp_x != 0 || t.vp_y != 0 || t.vp_w == 0 || t.vp_h == 0 || out_w == 0 || out_h == 0)
			return false;
		const auto off = [](uint32_t a, uint32_t b) { return a > b ? a - b : b - a; };
		if (presented_size(t.target.w, t.target.h, out_w, out_h) && off(t.vp_w, t.target.w) <= 1 && off(t.vp_h, t.target.h) <= 1)
			return true; // the exact-size case, unchanged
		if (t.vp_w > t.target.w || t.vp_h > t.target.h)
			return false; // the viewport does not fit inside its own target
		const auto pad = [](uint32_t size) { const uint32_t p = (size * 3 + 99) / 100; return p > 16 ? p : 16; };
		if (t.target.w - t.vp_w > pad(t.target.w) || t.target.h - t.vp_h > pad(t.target.h))
			return false; // more than a pool's padding between the viewport and its target
		if (t.vp_w * 4 < out_w)
			return false; // under 1/4 of the presented width
		const double a = double(t.vp_w) / double(t.vp_h), b = double(out_w) / double(out_h);
		return std::abs(a - b) <= 0.01 * b;
	}

	/// A tonemapper BELOW the presented size is the main view's only when its scene colour is on the grid of the
	/// depth buffer ReShade's depth add-on picked: Unreal sizes the main view's scene colour and depth from one extent
	/// (Poppy Playtime: both 384x220 around a 384x217 image), while a scene capture's own tonemap (an in-game camera,
	/// a monitor, at the view's aspect) reads a scene colour of the capture's size. No depth buffer known (0x0): not
	/// the main view's - she stays at Present until one is.
	inline bool main_view_grid(const Tex &scene, uint32_t depth_w, uint32_t depth_h)
	{
		return depth_w != 0 && depth_h != 0 && scene.w == depth_w && scene.h == depth_h;
	}

	/// Of what the draw reads, its scene colour: the first HDR texture of the target's size (none: not a tonemapper -
	/// a UI quad, a copy, a fade). `ambiguous` is set when it reads more than one.
	inline Tex scene_read(const Tracker &t, const Tex *reads, size_t n, bool *ambiguous = nullptr)
	{
		Tex found;
		int count = 0;
		for (size_t i = 0; i < n; ++i)
			if (reads[i].kind == Kind::hdr && !reads[i].written && reads[i].id != t.target.id && reads[i].w == t.target.w && reads[i].h == t.target.h)
			{
				if (count == 0)
					found = reads[i];
				count += reads[i].id != found.id || count == 0;
			}
		if (ambiguous != nullptr)
			*ambiguous = count > 1;
		return found;
	}

	/// A COMPUTE tonemapper (an engine that runs its post-processing in compute shaders - Unreal 5 does): a dispatch
	/// that WRITES exactly one texture of the presented size, an 8/10-bit one (its output), and READS an HDR texture of
	/// that same size (its input, returned; none: not a tonemapper). A lighting or G-buffer pass reads 8/10-bit
	/// textures of that size too, but writes an HDR one (or several): never taken. `ambiguous` is set when it reads more
	/// than one such HDR texture.
	inline Tex compute_scene(const Tex *bound, size_t n, uint32_t out_w, uint32_t out_h, bool *ambiguous = nullptr)
	{
		Tex output;
		int outputs = 0;
		for (size_t i = 0; i < n; ++i)
			if (bound[i].written && presented_size(bound[i].w, bound[i].h, out_w, out_h))
			{
				++outputs;
				output = bound[i];
			}
		if (ambiguous != nullptr)
			*ambiguous = false;
		if (outputs != 1 || output.kind != Kind::ldr)
			return Tex();
		Tracker t;
		t.target = output;
		return scene_read(t, bound, n, ambiguous);
	}
}
