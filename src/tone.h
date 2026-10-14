#pragma once
// tone: her picture (8-bit sRGB, as her engine drew her for a screen) into a game's HDR scene colour, BEFORE its
// tonemapper - so that after the game's tone curve she looks as her engine drew her, and the game's own grade, bloom,
// vignette and grain then apply to her as to its world.
//
// The game's curve is not read: a filmic curve close to every Unreal game's default (ACES, Narkowicz's fit, with
// Unreal's 0.6 pre-scale) stands in, inverted. Unreal stores its scene colour PRE-EXPOSED (multiplied by its eye
// adaptation since 4.25: r.UsePreExposure, on by default), so what the tonemapper is left to apply is ~1: no exposure
// needs reading. `exposure` scales her in the scene for a game whose curve or exposure differs ([tone] Exposure).
//
// This file is the reference: AstraPassthrough.fx carries the same functions and constants in HLSL (tests/core_tests.cpp
// checks the constants match), tests check the round trip.
#include <algorithm>
#include <cmath>

namespace tone
{
	constexpr float kA = 2.51f, kB = 0.03f, kC = 2.43f, kD = 0.59f, kE = 0.14f; // ACES fit (Narkowicz 2015)
	constexpr float kPre = 0.6f;                                               // Unreal's scale into it
	constexpr float kTop = 0.98f; // the fit approaches a/c = 1.03 at infinity: her whitest white maps to this

	inline float fit(float x)
	{
		x = std::max(x, 0.0f);
		return std::clamp(x * (kA * x + kB) / (x * (kC * x + kD) + kE), 0.0f, 1.0f);
	}

	/// The scene value the fit maps to y (y in [0, kTop]; larger is clamped).
	inline float fit_inverse(float y)
	{
		y = std::clamp(y, 0.0f, kTop);
		// (c y - a) x^2 + (d y - b) x + e y = 0, the root >= 0 (c y - a < 0 below a / c)
		const float qa = kC * y - kA, qb = kD * y - kB, qc = kE * y;
		const float disc = std::max(qb * qb - 4.0f * qa * qc, 0.0f);
		return std::max((-qb - std::sqrt(disc)) / (2.0f * qa), 0.0f);
	}

	inline float srgb_decode(float c)
	{
		c = std::clamp(c, 0.0f, 1.0f);
		return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
	}

	inline float srgb_encode(float c)
	{
		c = std::clamp(c, 0.0f, 1.0f);
		return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
	}

	/// A display value (sRGB-encoded, as her picture holds it) -> the game's scene colour (linear, pre-exposed).
	inline float display_to_scene(float d, float exposure)
	{
		return fit_inverse(srgb_decode(d)) * exposure / kPre;
	}

	/// The game's scene colour -> about what its tonemapper will show (sRGB-encoded): the light and fog around her are
	/// measured in the same space as her picture.
	inline float scene_to_display(float s, float exposure)
	{
		return srgb_encode(fit(s * kPre / std::max(exposure, 1e-4f)));
	}
}
