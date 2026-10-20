#pragma once
// How stale the camera "the presented frame" uses (link.h's camera_delay) should be, measured instead of
// guessed. Portable C++ (no Windows): tests/ builds it natively.
//
// Why: camera_delay exists because a game's render thread presents a frame its game thread (and the
// provider riding it) has already moved past - the picture on screen should use the PROVIDER camera that
// was current when the game thread produced the frame now being presented, which is a SMALL, roughly
// constant amount of WALL-CLOCK TIME behind the newest provider camera (about one present's worth of
// render-thread buffering), not a fixed number of the provider's OWN pushes back.
//
// 0.2.4's bug: PushPace measured a RATE (presented frames per distinct push, a unitless ratio) and
// link.cpp fed that number straight into CameraRing::get() as a COUNT of ring slots to step back. Each
// slot itself spans about `gap` presents, so the wall-clock age of the picked camera grew like
// delay_slots * time_per_slot =~ gap^2 / present_fps - QUADRATIC in the push:present cadence ratio, not
// the intended ~1 present of staleness. A live GTA III DE / Mecha Chameleon log showed ~30% of presented
// frames had no fresh provider camera of their own even once (Unreal pushes once per UE4SS engine tick,
// which a present can easily outrun); at a push:present ratio of ~6-8 that bug alone would produce
// several hundred ms of camera staleness. (It was NOT the ~500 ms camera-turn lag the owner saw in Poppy
// Playtime, 2026-10-20: that game's own log shows a push on 81-98% of presents, a ratio near 1, where the
// old count and the new age pick the same camera - that lag was AstraUE's follow brain chasing a spot that
// turns with the view.) A live Scarlet Skips capture test, 2026-10-19, measured ~14% idle (push keeps up with
// present almost always there) and the SAME ~100 ms settled reprojection error at the old camera_delay 1
// and its old ceiling 3 alike - the ceiling, not the slider position, was the limit on that game, i.e.
// ~100 ms is already a MEASURED amount of staleness this mechanism can ask for without the picture
// visibly lagging a turn.
//
// The fix: PushPace now EMAs the wall-clock gap (ms) between distinct pushes, using each camera's
// `at_ms`. CameraRing::get_by_age (provider_core.h) picks the newest camera at least `target_age_ms()`
// old, falling back to the newest when none is - so a game whose provider pushes every present still
// gets ~1 push-gap of staleness (today's validated default), and a game whose provider pushes only every
// few presents is asked to look back AT MOST one push (never several, and never a multiple that grows
// with how rarely it pushes): the per-push age it lands on is however stale that single push already is,
// capped at kAgeCeilingMs so a very sparse provider is never asked to wait longer than the ~100 ms a live
// capture has already shown is enough to settle.
#include <algorithm>
#include <cstdint>

namespace camera_pace
{
	/// How long to let a sparsely-pushing provider's picked camera be before giving up and using it
	/// anyway: DERIVED from the live Scarlet Skips capture cited above (its reprojection error was
	/// already measured to settle at ~100 ms; asking a slower provider to wait longer than that buys
	/// nothing a live capture has shown), not a fresh guess.
	constexpr int kAgeCeilingMs = 100;
	/// The assumed staleness before anything is measured: about one present at a typical 60 Hz Unreal
	/// tick - the same case the old kDefaultDelay=1 (one ring slot, at a push-per-present provider)
	/// covered, so an unmeasured provider behaves close to what it did before this existed.
	constexpr int kDefaultTargetAgeMs = 17;
	/// How quickly the average reacts to a change (smaller = steadier, bigger = catches a framerate
	/// change sooner); 20-ish pushes' worth of memory, a couple of seconds for a 60 Hz Unreal tick.
	constexpr float kEmaAlpha = 0.05f;
	/// A push gap longer than this (ms) is a pause, not a cadence: left out of the average.
	constexpr int kPauseMs = 250;

	/// Tracks, as an exponential moving average, the wall-clock milliseconds between distinct provider
	/// camera pushes - the age (ms) the shown camera should be picked at, absent a fresher signal (the
	/// present callback's own camera, link.cpp's `presented_now` path, always wins over this).
	struct PushPace
	{
		float gap_ms_ema = -1.0f; // -1: no push observed yet
		int64_t last_push_ms = -1;

		/// Call once per presented frame there is a provider camera at all. pushed: a provider camera
		/// with a NEW ring index arrived since the last call (link.cpp: `newest.index` changed).
		/// now_ms: the add-on's clock this present (provider::now_ms()).
		void tick(bool pushed, int64_t now_ms)
		{
			if (!pushed)
				return;
			if (last_push_ms >= 0)
			{
				const float gap = float(now_ms - last_push_ms);
				// a gap longer than kPauseMs is a pause (a menu, a load, the game paused), not the provider's cadence:
				// averaged in, one 17 s pause pulled the measure to the 100 ms ceiling and she was drawn with a
				// camera 100 ms old for the next second or two (Poppy Playtime log, 2026-10-20: "camera delay 100 ms,
				// the picture's camera is 17709 ms old", then "100 ms" again 15 s later)
				// (the first gap seeds the measure whatever it is, bounded by the ceiling, and a gap is a pause only
				// against the provider's OWN cadence too - a game that pushes every 330 ms for real is measured, not
				// left on the unmeasured default forever: review of 0.2.8)
				if (gap_ms_ema < 0.0f)
					gap_ms_ema = std::min(gap, float(kAgeCeilingMs));
				else if (gap <= std::max(float(kPauseMs), gap_ms_ema * 4.0f))
					gap_ms_ema += (gap - gap_ms_ema) * kEmaAlpha;
			}
			last_push_ms = now_ms;
		}

		/// The effective camera age (ms) to pick: about one measured push-gap (so a provider pushing
		/// every present reproduces the old "one ring slot back" behaviour almost exactly), clamped to
		/// kAgeCeilingMs so a provider that pushes rarely is never asked to look back several pushes'
		/// worth - just however stale its single most recent push already is, bounded.
		int target_age_ms() const
		{
			const float g = gap_ms_ema < 0.0f ? float(kDefaultTargetAgeMs) : gap_ms_ema;
			return int(std::clamp(g, 0.0f, float(kAgeCeilingMs)) + 0.5f);
		}
	};
}
