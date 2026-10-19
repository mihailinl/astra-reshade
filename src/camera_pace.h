#pragma once
// How many ring slots back "the presented camera" (link.h's camera_delay) should look, measured instead of
// guessed. Portable C++ (no Windows): tests/ builds it natively.
//
// Why: camera_delay exists because a game's render thread presents a frame its game thread (and the
// provider riding it) has already moved past - the picture on screen is the newest PROVIDER camera's
// picture from `camera_delay` pushes ago. A fixed default (1) and a fixed ceiling (3) assume the provider
// pushes a fresh camera about as often as the game presents one; Unreal pushes once per UE4SS engine tick,
// which a present can easily outrun (a live GTA III DE / Mecha Chameleon log: ~30% of presented frames had
// no fresh provider camera of their own even once; a live Scarlet Skips capture test, 2026-10-19, measured
// ~14% idle and the SAME ~100 ms settled reprojection error at camera_delay 1 and 3 alike - the ceiling,
// not the slider position, was the limit). PushPace measures the real gap and removes the guess.
#include <algorithm>

namespace camera_pace
{
	/// Was camera_delay's hard ceiling (and the ImGui slider's); proven too low live, 2026-10-19 (see above) -
	/// 3 ring slots is ~50 ms at 60 Hz, and the live test still needed more than that to settle.
	constexpr int kMaxDelay = 8;
	/// camera_delay's old fixed default - PushPace converges here when a push lands on (almost) every present,
	/// so a game already well served by the old default sees no change.
	constexpr int kDefaultDelay = 1;
	/// How quickly the average reacts to a change (smaller = steadier, bigger = catches a framerate change
	/// sooner); 20-ish pushes' worth of memory, which is a couple of seconds for a 60 Hz Unreal tick.
	constexpr float kEmaAlpha = 0.05f;

	/// Tracks, as an exponential moving average, how many presented frames pass between distinct provider
	/// camera pushes - the ring slots "host" should look back absent a fresher signal (the present callback's
	/// own camera, link.cpp's `presented_now` path, always wins over this when it is available).
	struct PushPace
	{
		float gap_ema = float(kDefaultDelay);
		int frames_since_push = -1; // -1: no push observed yet; the gap before the very first one is unknown

		/// Call once per presented frame. pushed: a provider camera with a NEW ring index arrived since the
		/// last call (link.cpp: `newest.index` changed). Never called when there is no provider camera at all.
		void tick(bool pushed)
		{
			if (!pushed)
			{
				if (frames_since_push >= 0)
					++frames_since_push;
				return;
			}
			if (frames_since_push >= 0)
			{
				const float gap = float(frames_since_push + 1);
				gap_ema += (gap - gap_ema) * kEmaAlpha;
			}
			frames_since_push = 0;
		}

		/// The effective camera_delay (ring slots back), rounded and clamped to kMaxDelay (never negative).
		int delay() const
		{
			const int d = int(gap_ema + 0.5f);
			return std::clamp(d, 0, kMaxDelay);
		}
	};
}
