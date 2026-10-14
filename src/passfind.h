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
		Tex target;                  // the first colour target bound (id 0: none)
		uint32_t targets_bound = 0;  // how many
		uint32_t vp_w = 0, vp_h = 0; // viewport 0
		bool in_pass = false;        // inside an API render pass: nothing can be copied there

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
		void viewport(float w, float h)
		{
			vp_w = w > 0.0f ? uint32_t(w + 0.5f) : 0;
			vp_h = h > 0.0f ? uint32_t(h + 0.5f) : 0;
		}
	};

	/// The target is the presented picture's size, or the same aspect and at least half of it (a game that tonemaps
	/// at its render resolution and upscales after).
	inline bool presented_size(uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h)
	{
		if (out_w == 0 || out_h == 0 || w == 0 || h == 0)
			return false;
		if (w == out_w && h == out_h)
			return true;
		const double a = double(w) / double(h), b = double(out_w) / double(out_h);
		return std::abs(a - b) <= 0.01 * b && w * 2 >= out_w && w <= out_w;
	}

	/// Could this draw be the tonemapper by its SHAPE (a full-screen draw into one 8/10-bit target of the presented
	/// size)? Cheap: asked of every draw before what it reads is looked up. `vertices`: its vertex (or index) count.
	inline bool tonemapper_shape(const Tracker &t, uint32_t vertices, uint32_t instances, uint32_t out_w, uint32_t out_h)
	{
		if (t.in_pass || t.target.id == 0 || t.target.kind != Kind::ldr)
			return false;
		if ((vertices != 3 && vertices != 4 && vertices != 6) || instances != 1)
			return false;
		const auto off = [](uint32_t a, uint32_t b) { return a > b ? a - b : b - a; };
		if (off(t.vp_w, t.target.w) > 1 || off(t.vp_h, t.target.h) > 1)
			return false;
		return presented_size(t.target.w, t.target.h, out_w, out_h);
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
