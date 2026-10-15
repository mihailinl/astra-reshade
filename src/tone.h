#pragma once
// tone: her picture (8-bit sRGB, as her engine drew her for a screen) into a game's HDR scene colour, BEFORE its
// tonemapper - so that after the game's tone curve she looks as her engine drew her, and the game's own grade, bloom,
// vignette and grain then apply to her as to its world.
//
// The game's curve is not read: Unreal's DEFAULT one stands in (the film tonemapper of UE 4.15+ and UE 5 with its
// default settings: FilmSlope 0.88, Toe 0.55, Shoulder 0.26, BlackClip 0, WhiteClip 0.04, BlueCorrection 0.6,
// ExpandGamut 1 - see ue_reference below, a port of it). A game that changes those settings (a post-process volume's
// Film or Color Grading values, its own LUT) is NOT matched: [tone] Exposure trims her brightness, nothing more.
// Unreal stores its scene colour PRE-EXPOSED (multiplied by its eye adaptation since 4.25: r.UsePreExposure, on by
// default), so what the tonemapper is left to apply is ~1: no exposure needs reading.
//
// What is inverted (the "model"): sRGB -> AP1, the gamut expansion of bright saturated colours (by fixed-point
// iteration), blue correction, the pre-desaturation (0.96), the film curve per channel, the post-desaturation (0.93),
// blue "uncorrection", AP1 -> sRGB. Its matrices go to AstraPassthrough.fx as uniforms and its curve as a 1D table (a
// texture the add-on makes from this file), so the effect carries no constant of its own. NOT inverted (they stay in
// the reference; the tests measure what they cost - a few hundredths on reds and dark saturated colours): ACES's glow
// and red modifier.
//
// tests/core_tests.cpp: the model's round trip, and the model's inverse through this port of Unreal's whole default
// chain across a colour grid (greys, skin, saturated colours).
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace tone
{
	// ---- 3x3 matrices (row-major, M * v), double --------------------------------------------------------------

	struct M3
	{
		double m[3][3];
	};

	inline M3 mul(const M3 &a, const M3 &b)
	{
		M3 r{};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
		return r;
	}

	inline void apply(const M3 &a, const double v[3], double out[3])
	{
		double r[3];
		for (int i = 0; i < 3; ++i)
			r[i] = a.m[i][0] * v[0] + a.m[i][1] * v[1] + a.m[i][2] * v[2];
		out[0] = r[0], out[1] = r[1], out[2] = r[2];
	}

	inline M3 inverse(const M3 &a)
	{
		const auto &m = a.m;
		const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1], c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2], c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
		const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
		M3 r{};
		r.m[0][0] = c00 / det;
		r.m[1][0] = c01 / det;
		r.m[2][0] = c02 / det;
		r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
		r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
		r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
		r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
		r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
		r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
		return r;
	}

	/// lerp(I, b, k) = (1 - k) I + k b
	inline M3 toward(const M3 &b, double k)
	{
		M3 r{};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				r.m[i][j] = (i == j ? 1.0 - k : 0.0) + k * b.m[i][j];
		return r;
	}

	// ---- Unreal's colour spaces (Engine/Shaders/Private/ACES.ush, TonemapCommon.ush) ---------------------------

	constexpr M3 kSrgbToXyz = {{{0.4124564, 0.3575761, 0.1804375}, {0.2126729, 0.7151522, 0.0721750}, {0.0193339, 0.1191920, 0.9503041}}};
	constexpr M3 kXyzToSrgb = {{{3.2409699419, -1.5373831776, -0.4986107603}, {-0.9692436363, 1.8759675015, 0.0415550574},
		{0.0556300797, -0.2039769589, 1.0569715142}}};
	constexpr M3 kD65ToD60 = {{{1.01303, 0.00610531, -0.014971}, {0.00769823, 0.998165, -0.00503203}, {-0.00284131, 0.00468516, 0.924507}}};
	constexpr M3 kD60ToD65 = {{{0.987224, -0.00611327, 0.0159533}, {-0.00759836, 1.00186, 0.00533002}, {0.00307257, -0.00509595, 1.08168}}};
	constexpr M3 kXyzToAp1 = {{{1.6410233797, -0.3248032942, -0.2364246952}, {-0.6636628587, 1.6153315917, 0.0167563477},
		{0.0117218943, -0.0082844420, 0.9883948585}}};
	constexpr M3 kAp1ToXyz = {{{0.6624541811, 0.1340042065, 0.1561876870}, {0.2722287168, 0.6740817658, 0.0536895174},
		{-0.0055746495, 0.0040607335, 1.0103391003}}};
	constexpr M3 kAp0ToXyz = {{{0.9525523959, 0.0, 0.0000936786}, {0.3439664498, 0.7281660966, -0.0721325464}, {0.0, 0.0, 1.0088251844}}};
	constexpr M3 kXyzToAp0 = {{{1.0498110175, 0.0, -0.0000974845}, {-0.4959030231, 1.3733130458, 0.0982400361}, {0.0, 0.0, 0.9912520182}}};
	// ACES's "blue light artifact fix" (an LMT, in AP0), and its inverse
	constexpr M3 kBlueCorrect = {{{0.9404372683, -0.0183068787, 0.0778696104}, {0.0083786969, 0.8286599939, 0.1629613092},
		{0.0005471261, -0.0008833746, 1.0003362486}}};
	constexpr M3 kBlueCorrectInv = {{{1.06318, 0.0233956, -0.0865726}, {-0.0106337, 1.20632, -0.19569}, {-0.000590887, 0.00105248, 0.999538}}};
	// ExpandGamut's "wide" primaries (between P3 and AP1), D65
	constexpr M3 kWideToXyz = {{{0.5441691, 0.2395926, 0.1666943}, {0.2394656, 0.7021530, 0.0583814}, {-0.0023439, 0.0361834, 1.0552183}}};
	constexpr double kAp1Y[3] = {0.2722287168, 0.6740817658, 0.0536895174}; // AP1_RGB2Y

	inline M3 srgb_to_ap1()
	{
		return mul(kXyzToAp1, mul(kD65ToD60, kSrgbToXyz));
	}
	inline M3 ap1_to_srgb()
	{
		return mul(kXyzToSrgb, mul(kD60ToD65, kAp1ToXyz));
	}

	/// Unreal's film settings (FPostProcessSettings), its defaults.
	struct Film
	{
		double slope = 0.88, toe = 0.55, shoulder = 0.26, black = 0.0, white = 0.04;
		double blue_correction = 0.6, expand_gamut = 1.0;
	};

	/// The film curve on one channel of the working colour (AP1, after the pre-desaturation): FilmToneMap's
	/// toe / straight / shoulder in log10, blended across the gap between the toe's and the shoulder's match points.
	inline double film_curve(double x, const Film &f = Film())
	{
		const double toe_scale = 1.0 + f.black - f.toe, shoulder_scale = 1.0 + f.white - f.shoulder;
		const double in_match = 0.18, out_match = 0.18;
		double toe_match;
		if (f.toe > 0.8)
			toe_match = (1.0 - f.toe - out_match) / f.slope + std::log10(in_match);
		else
		{
			const double bt = (out_match + f.black) / toe_scale - 1.0;
			toe_match = std::log10(in_match) - 0.5 * std::log((1.0 + bt) / (1.0 - bt)) * (toe_scale / f.slope);
		}
		const double straight_match = (1.0 - f.toe) / f.slope - toe_match;
		const double shoulder_match = f.shoulder / f.slope - straight_match;
		if (!(x > 0.0))
			return 0.0; // log10(0) = -inf: the toe's limit
		const double l = std::log10(x);
		const double straight = f.slope * (l + straight_match);
		double toe = -f.black + (2.0 * toe_scale) / (1.0 + std::exp((-2.0 * f.slope / toe_scale) * (l - toe_match)));
		double shoulder = (1.0 + f.white) - (2.0 * shoulder_scale) / (1.0 + std::exp((2.0 * f.slope / shoulder_scale) * (l - shoulder_match)));
		toe = l < toe_match ? toe : straight;
		shoulder = l > shoulder_match ? shoulder : straight;
		double t = std::clamp((l - toe_match) / (shoulder_match - toe_match), 0.0, 1.0);
		t = shoulder_match < toe_match ? 1.0 - t : t;
		t = (3.0 - 2.0 * t) * t * t;
		return toe + (shoulder - toe) * t;
	}

	/// The working value the curve maps to y (bisection in log10; y beyond the curve's range is clamped).
	inline double film_curve_inverse(double y, const Film &f = Film())
	{
		if (!(y > 0.0))
			return 0.0;
		double lo = -12.0, hi = 6.0;
		for (int i = 0; i < 80; ++i)
		{
			const double mid = 0.5 * (lo + hi);
			(film_curve(std::pow(10.0, mid), f) < y ? lo : hi) = mid;
		}
		return std::pow(10.0, 0.5 * (lo + hi));
	}

	// ---- the reference: Unreal's whole default tonemapping of a scene colour, as its colour LUT bakes it -------

	namespace aces
	{
		inline double saturation(const double c[3])
		{
			const double mn = std::min({c[0], c[1], c[2]}), mx = std::max({c[0], c[1], c[2]});
			return (std::max(mx, 1e-10) - std::max(mn, 1e-10)) / std::max(mx, 1e-2);
		}
		inline double yc(const double c[3])
		{
			const double r = c[0], g = c[1], b = c[2];
			const double chroma = std::sqrt(std::max(b * (b - g) + g * (g - r) + r * (r - b), 0.0));
			return (b + g + r + 1.75 * chroma) / 3.0;
		}
		inline double sigmoid_shaper(double x)
		{
			const double t = std::max(1.0 - std::fabs(0.5 * x), 0.0);
			const double y = 1.0 + (x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0) * (1.0 - t * t);
			return 0.5 * y;
		}
		inline double glow_fwd(double yc_in, double gain, double mid)
		{
			if (yc_in <= 2.0 / 3.0 * mid)
				return gain;
			if (yc_in >= 2.0 * mid)
				return 0.0;
			return gain * (mid / yc_in - 0.5);
		}
		inline double hue(const double c[3])
		{
			if (c[0] == c[1] && c[1] == c[2])
				return 0.0;
			double h = (180.0 / 3.14159265358979323846) * std::atan2(std::sqrt(3.0) * (c[1] - c[2]), 2.0 * c[0] - c[1] - c[2]);
			if (h < 0.0)
				h += 360.0;
			return std::clamp(h, 0.0, 360.0);
		}
		inline double center_hue(double h, double center)
		{
			double c = h - center;
			if (c < -180.0)
				c += 360.0;
			else if (c > 180.0)
				c -= 360.0;
			return c;
		}
	}

	/// FilmToneMap (TonemapCommon.ush): AP1 in, AP1 out - the glow and red modifier (AP0), the pre-desaturation, the
	/// curve, the post-desaturation.
	inline void film_tonemap(const double in[3], double out[3], const Film &f = Film())
	{
		double c0[3];
		apply(mul(kXyzToAp0, kAp1ToXyz), in, c0);
		const double sat = aces::saturation(c0);
		const double s = aces::sigmoid_shaper((sat - 0.4) / 0.2);
		const double glow = 1.0 + aces::glow_fwd(aces::yc(c0), 0.05 * s, 0.08);
		for (double &v : c0)
			v *= glow;
		const double centered = aces::center_hue(aces::hue(c0), 0.0);
		double w = std::clamp(1.0 - std::fabs(2.0 * centered / 135.0), 0.0, 1.0);
		w = w * w * (3.0 - 2.0 * w);
		c0[0] += w * w * sat * (0.03 - c0[0]) * (1.0 - 0.82);
		double wc[3];
		apply(mul(kXyzToAp1, kAp0ToXyz), c0, wc);
		for (double &v : wc)
			v = std::max(v, 0.0);
		double y = wc[0] * kAp1Y[0] + wc[1] * kAp1Y[1] + wc[2] * kAp1Y[2];
		for (double &v : wc)
			v = y + (v - y) * 0.96;
		double t[3];
		for (int i = 0; i < 3; ++i)
			t[i] = film_curve(wc[i], f);
		y = t[0] * kAp1Y[0] + t[1] * kAp1Y[1] + t[2] * kAp1Y[2];
		for (int i = 0; i < 3; ++i)
			out[i] = std::max(y + (t[i] - y) * 0.93, 0.0);
	}

	inline double srgb_decode(double c)
	{
		c = std::clamp(c, 0.0, 1.0);
		return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
	}

	inline double srgb_encode(double c)
	{
		c = std::clamp(c, 0.0, 1.0);
		return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
	}

	/// The blue correction's matrices in AP1 (BlueCorrectAP1 and its inverse).
	inline M3 blue_ap1()
	{
		return mul(mul(kXyzToAp1, kAp0ToXyz), mul(kBlueCorrect, mul(kXyzToAp0, kAp1ToXyz)));
	}
	inline M3 blue_inv_ap1()
	{
		return mul(mul(kXyzToAp1, kAp0ToXyz), mul(kBlueCorrectInv, mul(kXyzToAp0, kAp1ToXyz)));
	}

	/// Unreal's default tonemapping (PostProcessCombineLUTs.usf, sRGB output, every other setting at its default): a
	/// scene colour (linear, sRGB primaries, pre-exposed) -> what the screen shows (sRGB-encoded).
	inline void ue_reference(const double scene[3], double display[3], const Film &f = Film())
	{
		double c[3];
		apply(srgb_to_ap1(), scene, c);
		// the gamut expansion of bright saturated colours
		const double luma = c[0] * kAp1Y[0] + c[1] * kAp1Y[1] + c[2] * kAp1Y[2];
		if (luma > 1e-10)
		{
			double dist = 0.0;
			for (double v : c)
				dist += (v / luma - 1.0) * (v / luma - 1.0);
			const double amount = (1.0 - std::exp2(-4.0 * dist)) * (1.0 - std::exp2(-4.0 * f.expand_gamut * luma * luma));
			double e[3];
			apply(mul(mul(kXyzToAp1, kWideToXyz), ap1_to_srgb()), c, e);
			for (int i = 0; i < 3; ++i)
				c[i] += (e[i] - c[i]) * amount;
		}
		apply(toward(blue_ap1(), f.blue_correction), c, c);
		film_tonemap(c, c, f);
		apply(toward(blue_inv_ap1(), f.blue_correction), c, c);
		apply(ap1_to_srgb(), c, c);
		for (int i = 0; i < 3; ++i)
			display[i] = srgb_encode(std::max(c[i], 0.0));
	}

	// ---- the model: what is inverted, and what AstraPassthrough.fx computes ------------------------------------

	/// The model's matrices: scene (sRGB primaries) <-> AP1, the gamut expansion's target, AP1 -> working colour (blue
	/// correction, pre-desaturation) before the curve, the curve's output -> the shown linear colour (post-
	/// desaturation, blue uncorrection, AP1 -> sRGB), and the inverses of the last two.
	struct Matrices
	{
		M3 to_ap1, from_ap1, expand, fwd_in, inv_out, fwd_out, inv_in;
	};

	inline Matrices matrices(const Film &f = Film())
	{
		// lerp(dot(c, Y), c, k) as a matrix: k I + (1 - k) * 1 Y^T
		const auto desaturate = [](double k) {
			M3 d{};
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					d.m[i][j] = (i == j ? k : 0.0) + (1.0 - k) * kAp1Y[j];
			return d;
		};
		Matrices m;
		m.to_ap1 = srgb_to_ap1();
		m.from_ap1 = inverse(m.to_ap1);
		m.expand = mul(mul(kXyzToAp1, kWideToXyz), ap1_to_srgb());
		m.fwd_in = mul(desaturate(0.96), toward(blue_ap1(), f.blue_correction));
		m.inv_out = inverse(m.fwd_in);
		m.fwd_out = mul(ap1_to_srgb(), mul(toward(blue_inv_ap1(), f.blue_correction), desaturate(0.93)));
		m.inv_in = inverse(m.fwd_out);
		return m;
	}

	/// How far ExpandGamut moves an AP1 colour toward the wide primaries: bright saturated colours most.
	inline double expand_amount(const double c[3], double expand_gamut)
	{
		const double luma = c[0] * kAp1Y[0] + c[1] * kAp1Y[1] + c[2] * kAp1Y[2];
		if (!(luma > 1e-6))
			return 0.0;
		double dist = 0.0;
		for (int i = 0; i < 3; ++i)
			dist += (c[i] / luma - 1.0) * (c[i] / luma - 1.0);
		return (1.0 - std::exp2(-4.0 * dist)) * (1.0 - std::exp2(-4.0 * expand_gamut * luma * luma));
	}

	/// The gamut expansion's inverse by fixed-point iteration (kUnexpand steps: the effect unrolls them).
	constexpr int kUnexpand = 6;
	inline void unexpand(const Matrices &m, double c[3], double expand_gamut)
	{
		const double target[3] = {c[0], c[1], c[2]};
		for (int k = 0; k < kUnexpand; ++k)
		{
			double e[3];
			apply(m.expand, c, e);
			const double a = expand_amount(c, expand_gamut);
			for (int i = 0; i < 3; ++i)
				e[i] = target[i] - a * (e[i] - c[i]);
			c[0] = e[0], c[1] = e[1], c[2] = e[2];
		}
	}

	// The curve as a table (a 1024 x 2 R32F texture in the effect): row 0 the inverse - log10 of the working value
	// at y = (i / (N-1))^2 * kYMax (squared: fine steps in the darks) - and row 1 the curve at log10 x = kLogMin..kLogMax.
	constexpr int kLutN = 1024;
	constexpr float kYMax = 1.035f; // just under the curve's top (1 + WhiteClip): her white (about 1) is well inside
	constexpr float kLogMin = -8.0f, kLogMax = 3.0f;

	/// Fills `lut` (2 * kLutN floats: the inverse row, then the curve row).
	inline void fill_lut(float *lut, const Film &f = Film())
	{
		for (int i = 0; i < kLutN; ++i)
		{
			const double u = double(i) / double(kLutN - 1);
			const double y = u * u * double(kYMax);
			lut[i] = i == 0 ? kLogMin : float(std::log10(film_curve_inverse(y, f)));
			const double l = double(kLogMin) + (double(kLogMax) - double(kLogMin)) * u;
			lut[kLutN + i] = float(film_curve(std::pow(10.0, l), f));
		}
	}

	/// A row of the table at texel position `pos` (0..N-1), linearly between texels: the effect's tone_lut.
	inline float lut_at(const float *row, float pos)
	{
		pos = std::clamp(pos, 0.0f, float(kLutN - 1));
		const float i = std::min(std::floor(pos), float(kLutN - 2));
		const float a = row[int(i)], b = row[int(i) + 1];
		return a + (b - a) * (pos - i);
	}

	inline float curve_from_lut(const float *lut, float x)
	{
		if (!(x > 0.0f))
			return 0.0f;
		const float l = std::log2(x) * 0.30103f;
		return lut_at(lut + kLutN, (l - kLogMin) / (kLogMax - kLogMin) * float(kLutN - 1));
	}

	inline float curve_inverse_from_lut(const float *lut, float y)
	{
		if (!(y > 0.0f))
			return 0.0f;
		const float pos = std::sqrt(std::clamp(y / kYMax, 0.0f, 1.0f)) * float(kLutN - 1);
		return std::exp2(lut_at(lut, pos) * 3.32192809f);
	}

	/// Her shown colour (sRGB-encoded, as her picture holds it) -> the game's scene colour (linear, pre-exposed), times
	/// her exposure trim ([tone] Exposure). The effect's display_to_scene. A colour the game's curve cannot show from a
	/// scene colour (bright and saturated: the curve's pre-desaturation lifts the other channels toward white) comes out
	/// lighter, as the game's own bright saturated colours do.
	inline void display_to_scene(const float *lut, const Matrices &m, const float d[3], float exposure, float out[3], const Film &f = Film())
	{
		double c[3] = {srgb_decode(d[0]), srgb_decode(d[1]), srgb_decode(d[2])};
		apply(m.inv_in, c, c);
		for (int i = 0; i < 3; ++i)
			c[i] = curve_inverse_from_lut(lut, std::clamp(float(c[i]), 0.0f, kYMax));
		apply(m.inv_out, c, c);
		unexpand(m, c, f.expand_gamut);
		apply(m.from_ap1, c, c);
		for (int i = 0; i < 3; ++i)
			out[i] = float(std::max(c[i], 0.0)) * exposure;
	}

	/// The game's scene colour -> what its tonemapper (the model) will show there (sRGB-encoded): the light and fog
	/// around her are measured in her picture's space. Her exposure trim does not enter: it is hers, not the world's.
	/// The effect's host_display.
	inline void scene_to_display(const float *lut, const Matrices &m, const float s[3], float out[3], const Film &f = Film())
	{
		double c[3] = {s[0], s[1], s[2]}, e[3];
		apply(m.to_ap1, c, c);
		apply(m.expand, c, e);
		const double a = expand_amount(c, f.expand_gamut);
		for (int i = 0; i < 3; ++i)
			c[i] += (e[i] - c[i]) * a;
		apply(m.fwd_in, c, c);
		for (int i = 0; i < 3; ++i)
			c[i] = curve_from_lut(lut, float(std::max(c[i], 0.0)));
		apply(m.fwd_out, c, c);
		for (int i = 0; i < 3; ++i)
			out[i] = float(srgb_encode(c[i]));
	}

	/// The uniforms AstraPassthrough.fx takes from this file (a name and three floats: the matrices' rows, AP1's luma
	/// weights), set before every run of the effects. tests/core_tests.cpp checks the effect declares every one.
	struct Uniform
	{
		const char *name;
		float v[3];
	};
	constexpr size_t kUniforms = 22;
	inline void uniforms(Uniform out[kUniforms], const Film &f = Film())
	{
		const Matrices m = matrices(f);
		const M3 *all[7] = {&m.to_ap1, &m.from_ap1, &m.expand, &m.fwd_in, &m.inv_out, &m.fwd_out, &m.inv_in};
		static const char *const names[kUniforms] = {"ToneToAp1_0", "ToneToAp1_1", "ToneToAp1_2", "ToneFromAp1_0", "ToneFromAp1_1", "ToneFromAp1_2",
			"ToneExpand0", "ToneExpand1", "ToneExpand2", "ToneFwdIn0", "ToneFwdIn1", "ToneFwdIn2", "ToneInvOut0", "ToneInvOut1", "ToneInvOut2",
			"ToneFwdOut0", "ToneFwdOut1", "ToneFwdOut2", "ToneInvIn0", "ToneInvIn1", "ToneInvIn2", "ToneLuma"};
		for (size_t i = 0; i < kUniforms; ++i)
		{
			out[i].name = names[i];
			for (int j = 0; j < 3; ++j)
				out[i].v[j] = i < 21 ? float(all[i / 3]->m[i % 3][j]) : float(kAp1Y[j]);
		}
	}
	/// ...and four more numbers (ToneLut: the table's y max, log10 min, log10 max, and ExpandGamut).
	inline void lut_shape(float out[4], const Film &f = Film())
	{
		out[0] = kYMax, out[1] = kLogMin, out[2] = kLogMax, out[3] = float(f.expand_gamut);
	}
}
