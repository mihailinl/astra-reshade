#pragma once
// pretone's rules, portable (no ReShade, no Windows): tests/core_tests.cpp runs them over recorded sequences.
//   choose()   where her pass is drawn: before the game's tonemapper only in a game whose engine's tonemapper (its
//              shape: passfind.h) and tone curve (tone.h) this add-on knows - Unreal - as the provider or the launcher's
//              ini declares; everywhere else at Present, as before pretone existed
//   Fallback   her pass at the tonemapper that did not draw: the Present path for a while, then a new try
//   Stability  a sub-resolution tonemapper (passfind.h) is drawn into only once its size has held for 60 found
//              frames running; after 3 changes in a session, dynamic resolution gives up and stays at Present
//   Retired    a copy of the scene colour no longer used is destroyed frames later, never while the GPU may read it
//   Staging    the upload buffers her picture goes through on a recording thread: reused only as late
//   CopyRing   D3D12: which descriptor copies a draw reads - only the ones made since its command list's previous draw
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "passfind.h"

namespace pretone_rules
{
	/// The engines whose tonemapper this add-on can find and whose tone curve it inverts: Unreal 4.15+ and 5 (the film
	/// tonemapper, tone.h). An engine named by a provider is a raw fact; what it allows is decided here.
	inline bool known_engine(const std::string &engine)
	{
		std::string e;
		for (const char c : engine)
			e.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
		return e == "unreal";
	}

	enum class Where : uint8_t
	{
		present, // over the presented picture (and over the game's UI)
		before,  // into the game's HDR scene colour, before its tonemapper (its grade, bloom and UI apply to her)
	};

	struct Facts
	{
		bool path_present = false;     // [tone] Path=present (ASTRA_TONE_PATH)
		std::string declared;          // the provider's engine (astra_provider_engine, pipe line "E"); "" none
		std::string ini;               // [tone] Engine= of the launcher's ini (ASTRA_TONE_ENGINE); "" none
		bool hdr_output = false;       // the game presents HDR
		bool present_callback = false; // the provider writes its camera at presentation: the tonemapper is too early
	};

	struct Choice
	{
		Where where;
		const char *why;
	};

	inline Choice choose(const Facts &f)
	{
		if (f.path_present)
			return {Where::present, "[tone] Path=present"};
		if (f.hdr_output)
			return {Where::present, "the game presents HDR"};
		const std::string &engine = !f.declared.empty() ? f.declared : f.ini;
		if (engine.empty())
			return {Where::present, "no game engine declared (by the provider, or [tone] Engine=)"};
		if (!known_engine(engine))
			return {Where::present, "the declared engine's tonemapper is not known to this add-on"};
		if (f.present_callback)
			return {Where::present, "the provider reads its camera at presentation"};
		return {Where::before, "an Unreal game"};
	}

	/// Her pass at the tonemapper that did not draw: ReShade marks the frame's effects drawn BEFORE it checks they are
	/// ready (compiling for the scene colour's format, reloading), so that frame nothing is drawn - neither she nor the
	/// user's other effects, at the tonemapper or at Present. After such a miss the Present path is held (1 s, doubling
	/// to 16 s while misses repeat) - the effects compile meanwhile - and the tonemapper is tried again; after kGiveUp
	/// misses in a row, the Present path for the rest of the session. One frame per try. Held by the CLOCK, never by a
	/// frame count: a compile takes seconds whatever the frame rate, and Poppy Playtime's menu at 700 frames a second
	/// spent all 8 tries of a 60-frame hold in 5 s, before its 6.3 s compile for a 384x217 scene colour was done.
	struct Fallback
	{
		static constexpr uint64_t kFirstHold = 1000, kMaxHold = 16000; // ms
		static constexpr int kGiveUp = 8;
		uint64_t until = 0, hold = 0; // ms
		int misses = 0;
		bool given_up = false;

		/// The Present path is held at this time (ms, any monotonic clock).
		bool holding(uint64_t now_ms) const
		{
			return given_up || now_ms < until;
		}
		/// Her pass at the tonemapper at `now_ms` drew (her technique ran) or not. True: a hold began.
		bool result(uint64_t now_ms, bool drawn)
		{
			if (drawn)
			{
				misses = 0;
				hold = 0;
				return false;
			}
			++misses;
			hold = hold == 0 ? kFirstHold : std::min(hold * 2, kMaxHold);
			until = now_ms + hold;
			given_up = given_up || misses >= kGiveUp;
			return true;
		}
	};

	/// A sub-resolution tonemapper (its viewport below the presented size) is drawn into only once the SAME size has
	/// been found kStable frames running: a size caught mid-change (dynamic resolution, a menu that drops screen
	/// percentage) would make a copy and an effect permutation for a target about to be replaced. A different size
	/// restarts the count (the existing Retired<> machinery in pretone.cpp retires the old copy the next time
	/// ensure_copy is asked for the new one - ready() only decides WHEN to ask). After kMaxChanges sizes have been
	/// tried in one session, further changes are not: dynamic resolution would otherwise retry forever. The exact
	/// presented size needs none of this - today's behaviour, no wait, never given up.
	struct Stability
	{
		static constexpr uint32_t kStable = 60;
		static constexpr int kMaxChanges = 3;
		uint32_t w = 0, h = 0; // the sub-resolution size counted so far (meaningless until have_size)
		uint32_t count = 0;    // consecutive found frames of that size
		int changes = 0;       // how many times the size has changed this session (the first size is not a change)
		bool have_size = false, given_up = false;

		/// A tonemapper found this frame, with viewport (vp_w, vp_h) into a target sized for the presented picture
		/// (out_w, out_h) exactly or below it. True: drawn into.
		bool ready(uint32_t vp_w, uint32_t vp_h, uint32_t out_w, uint32_t out_h)
		{
			if (vp_w == out_w && vp_h == out_h)
				return true; // the exact presented size: no wait, never given up
			if (given_up)
				return false;
			if (!have_size || vp_w != w || vp_h != h)
			{
				if (have_size && ++changes >= kMaxChanges)
					given_up = true;
				have_size = true;
				w = vp_w;
				h = vp_h;
				count = 0;
				if (given_up)
					return false;
			}
			return ++count >= kStable;
		}
	};

	/// Try her pass at the tonemapper this frame? Not while the Present path is held - and not while the user has
	/// ReShade's effects switched off (nothing would draw anywhere; counted as misses, that gave pretone up for good).
	inline bool should_try(const Fallback &f, uint64_t now_ms, bool effects_on)
	{
		return effects_on && !f.holding(now_ms);
	}

	/// How many presents the GPU may still be behind the CPU with something we handed it - frames in flight are 2-3 (a
	/// game's, vkd3d's): every resource of ours a frame used waits this long before it is reused or freed.
	constexpr uint64_t kGpuFrames = 8;

	/// GPU resources the game's frames may still read (a copy made for one scene colour size, its view): destroyed
	/// kFrames presents after they were last used, never at once (frames in flight are 2-3; kFrames is a margin).
	template <class T>
	struct Retired
	{
		static constexpr uint64_t kFrames = kGpuFrames;
		struct Item
		{
			T what;
			uint64_t last_used;
		};
		std::vector<Item> items;

		void retire(const T &what, uint64_t last_used)
		{
			items.push_back(Item{what, last_used});
		}
		/// Destroys (calls `destroy` with) the ones no frame in flight can use at frame `now`.
		template <class F>
		void collect(uint64_t now, F &&destroy)
		{
			for (size_t i = 0; i < items.size();)
				if (now >= items[i].last_used + kFrames)
				{
					destroy(items[i].what);
					items.erase(items.begin() + std::ptrdiff_t(i));
				}
				else
					++i;
		}
	};

	/// Upload buffers her pass fills on the CPU and a game's command list copies from on the GPU later: one is reused (or
	/// freed to grow) only kGpuFrames presents after the frame it was given to; kSlots = kGpuFrames + 1, so a pass a
	/// frame always finds one.
	struct Staging
	{
		static constexpr int kSlots = int(kGpuFrames) + 1;
		bool used[kSlots] = {};
		uint64_t used_at[kSlots] = {};

		/// A slot free at `frame`, or -1.
		int pick(uint64_t frame) const
		{
			for (int i = 0; i < kSlots; ++i)
				if (!used[i] || frame >= used_at[i] + kGpuFrames)
					return i;
			return -1;
		}
		void use(int i, uint64_t frame)
		{
			used[i] = true;
			used_at[i] = frame;
		}
	};

	// ---- what a draw reads: the descriptors the game copied into the shader-visible heap for it -----------------

	/// One copy (D3D12 CopyDescriptors, CPU to CPU into a shader-visible heap): `count` descriptors into slots
	/// `off`.. of `heap`, from slots `src_off`.. of `src_heap` (where a view's ORIGINAL CPU handle is).
	struct Copy
	{
		uint64_t heap = 0;
		uint32_t off = 0, count = 0;
		uint64_t src_heap = 0;
		uint32_t src_off = 0;
	};

	/// One thread's recent copies, newest last.
	struct CopyRing
	{
		static constexpr uint32_t kSize = 1024;
		Copy c[kSize];
		uint64_t next = 0;

		void push(const Copy &x)
		{
			c[next++ % kSize] = x;
		}
	};

	/// Where a command list's previous draw, dispatch or reset left the ring of the thread recording it.
	struct Mark
	{
		const CopyRing *ring = nullptr;
		uint64_t at = 0;
	};

	constexpr uint32_t kLookBack = 128; // a draw's own copies are a few; never more than this
	constexpr uint32_t kSpan = 32;      // slots of one table looked at

	/// How many of the ring's newest copies a draw of the list marked `m` may read: the ones made on this thread since
	/// that list's previous draw (a game copies a draw's descriptors right before it). Older copies may name views of
	/// textures freed since - ReShade never forgets a view, and asking a freed texture its size crashes. A list
	/// recorded on another thread than its mark's reads none (its draw is not taken).
	inline uint32_t window(const CopyRing &r, const Mark &m)
	{
		if (m.ring != &r || r.next < m.at)
			return 0;
		return uint32_t(std::min<uint64_t>(r.next - m.at, std::min<uint64_t>(kLookBack, CopyRing::kSize)));
	}

	/// A descriptor table bound for the draw: its heap and first slot.
	struct Table
	{
		uint64_t heap = 0; // 0: none bound at this parameter
		uint32_t base = 0;
	};

	/// A bound slot resolved to the descriptor copied there.
	struct Slot
	{
		uint32_t table;  // index into the tables given
		uint32_t slot;   // in that table
		uint64_t src_heap;
		uint32_t src_off;
	};

	/// The slots of the bound tables, through the newest `window` copies (the newest copy wins a slot). A table spans
	/// up to kSpan slots and ends where the next one bound from the same heap begins (a game lays one draw's tables
	/// side by side and declares more slots than it fills). Returns how many slots were written to `out`.
	inline size_t resolve(const CopyRing &r, uint32_t window, const Table *tables, size_t n_tables, Slot *out, size_t cap)
	{
		size_t n = 0;
		for (size_t pi = 0; pi < n_tables; ++pi)
		{
			if (tables[pi].heap == 0)
				continue;
			const uint32_t base = tables[pi].base;
			uint32_t end = base + kSpan;
			for (size_t q = 0; q < n_tables; ++q)
				if (q != pi && tables[q].heap == tables[pi].heap && tables[q].base > base && tables[q].base < end)
					end = tables[q].base;
			uint32_t seen = 0; // slots of this table resolved already (by a newer copy)
			for (uint32_t k = 0; k < window && k < r.next; ++k)
			{
				const Copy &c = r.c[(r.next - 1 - k) % CopyRing::kSize];
				if (c.heap != tables[pi].heap || c.count == 0 || c.off >= end || c.off + c.count <= base)
					continue;
				for (uint32_t j = 0; j < c.count; ++j)
				{
					const uint32_t slot = c.off + j;
					if (slot < base || slot >= end || (seen & (1u << (slot - base))) != 0)
						continue;
					seen |= 1u << (slot - base);
					if (c.src_heap != 0 && n < cap)
						out[n++] = Slot{uint32_t(pi), slot - base, c.src_heap, c.src_off + j};
				}
			}
		}
		return n;
	}
}
