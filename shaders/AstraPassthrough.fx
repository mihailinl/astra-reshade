// Astra (the Astra companion's avatar) in any game ReShade runs in. The astra-reshade add-on uploads her engine's
// newest picture of her (ASTRAWORLD: premultiplied colour, ASTRADEPTH: linear depth - only her rectangle of the
// picture when the engine crops, AstraRect says where) and the camera it was drawn from; this re-projects it onto
// the game's presented camera, hides her behind the game's surfaces (its depth buffer: reversed Z, as Unreal's),
// matches the game's light and fog a little, and casts her shadow from her view from the sun when there is one.
// Always enabled (annotation): AstraActive gates it, so the game passes through untouched while she isn't out.
// Two places it runs (the add-on picks, AstraHdr says which): onto the presented picture (8-bit, sRGB), or - in an
// Unreal game (its provider says so), when the add-on finds the game's tonemapper - into the game's HDR scene colour
// right before it (linear, pre-exposed): her picture is mapped into the scene's light there (the tone functions
// below), and the game's tone curve, grade, bloom and UI then apply to her as to its world. No texture here has mipmaps: ReShade makes them with a compute
// pass, and between a game's own draws that would clobber the game's compute state.
// Derived from the Death Stranding add-on's AstraPassthrough.fx (same author).
#include "ReShade.fxh"
// A game integration's hooks into this compositor, as data (its own AstraGame.fxh, shipped beside this file in place
// of the foundation's empty one). Each is optional:
//   ASTRA_GAME_UI <semantic> + ASTRA_GAME_UI_ACTIVE <uniform>: a game that draws its UI into a target of its own and
//     composites it with its scene in one pass leaves no picture without the UI for her to go under: the integration
//     binds that target to the texture semantic and sets the bool uniform of that name (in every effect); her picture
//     then gets the game's UI laid back over it.
//   ASTRA_OCCLUDER_TEX <texture>: another compositor in the same game (another ReShade mod drawing its own world into
//     the picture) shares this texture by name: x its coverage (> 0.5: drawn), y its depth in metres along the game
//     camera's view axis, RGBA16F at the back buffer's size. Where it is drawn nearer than her, she is hidden. The
//     integration sets AstraOccluderActive while that compositor has written it this frame.
#include "AstraGame.fxh"

texture AstraWorldTex : ASTRAWORLD;
texture AstraDepthTex : ASTRADEPTH;
sampler sAstraWorld { Texture = AstraWorldTex; AddressU = CLAMP; AddressV = CLAMP; };
sampler sAstraDepth { Texture = AstraDepthTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
// her depth seen from the sun (the engine's light view): metres along SunVDir from its near plane, 0 where she isn't
texture AstraSunTex : ASTRASUN;
sampler sAstraSun { Texture = AstraSunTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
// the game's fog, read off its own picture (dark channel prior): per cell of a coarse grid, an upper bound of the
// fog's density from how washed out the darkest detail is at that depth, and the haziest colours (the fog's colour)
#define FOG_W 64
#define FOG_H 36
texture FogSigmaTex { Width = FOG_W; Height = FOG_H; Format = R32F; };
sampler sFogSigma { Texture = FogSigmaTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
texture FogColourTex { Width = FOG_W; Height = FOG_H; Format = RGBA16F; };
sampler sFogColour { Texture = FogColourTex; AddressU = CLAMP; AddressV = CLAMP; };
// x: density (1/m), yzw: the fog's colour; smoothed over frames (FogPrevTex: last frame's)
texture FogParamsTex { Width = 1; Height = 1; Format = RGBA16F; };
sampler sFogParams { Texture = FogParamsTex; MinFilter = POINT; MagFilter = POINT; };
texture FogPrevTex { Width = 1; Height = 1; Format = RGBA16F; };
sampler sFogPrev { Texture = FogPrevTex; MinFilter = POINT; MagFilter = POINT; };
// the game's picture (as shown: sRGB) at quarter size, then 1/32 (the light around her) and its mean (its grade)
texture AstraLightTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA8; };
sampler sAstraLight { Texture = AstraLightTex; AddressU = CLAMP; AddressV = CLAMP; };
texture AstraLightBlurTex { Width = BUFFER_WIDTH / 32; Height = BUFFER_HEIGHT / 32; Format = RGBA16F; };
sampler sAstraLightBlur { Texture = AstraLightBlurTex; AddressU = CLAMP; AddressV = CLAMP; };
texture AstraLightMeanTex { Width = 1; Height = 1; Format = RGBA16F; };
sampler sAstraLightMean { Texture = AstraLightMeanTex; MinFilter = POINT; MagFilter = POINT; };
#ifdef ASTRA_GAME_UI
texture AstraGameUiTex : ASTRA_GAME_UI;
sampler sAstraGameUi { Texture = AstraGameUiTex; AddressU = CLAMP; AddressV = CLAMP; };
uniform bool ASTRA_GAME_UI_ACTIVE = false; // set by the integration: the target is bound this frame
uniform float GameUiGamma < ui_type = "drag"; ui_min = 0.2; ui_max = 3.0; ui_step = 0.01; ui_label = "Game UI gamma"; > = 1.0;
uniform float GameUiStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01; ui_label = "Game UI brightness"; > = 1.0;
#endif
#ifdef ASTRA_OCCLUDER_TEX
texture ASTRA_OCCLUDER_TEX { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler sAstraOccluder { Texture = ASTRA_OCCLUDER_TEX; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
uniform bool AstraOccluderActive = false; // set by the integration: the other compositor wrote it this frame
#endif

// Loop counts the shader compiler must not see. Under Proton, ReShade compiles with Wine's d3dcompiler_47 (vkd3d-shader)
// unless the game ships Microsoft's (most Unreal games ship none): it refuses the [fastopt] ReShade writes for [loop],
// and it unrolls every loop it can count - minutes for the march below. A count read from a uniform keeps each a real
// loop on every compiler (tools/winecheck.sh compiles this effect with Wine's).
uniform int3 AstraLoops < hidden = true; > = int3(24, 7, 3); // march steps, halvings, settling steps

// set by the add-on
uniform bool AstraActive = false;
// drawn into the game's HDR scene colour, before its tonemapper (else onto the presented picture)
uniform bool AstraHdr < hidden = true; > = false;
// [tone] Exposure: her brightness in the game's scene, before its tone curve (1: as her engine drew her)
uniform float ToneExposure < hidden = true; > = 1.0;
// near, far, flags (1: [0,1] depth, 2: rows bottom-up, 4: reversed Z, 8: depth is linear metres along the view axis)
uniform float3 AstraPlanes = float3(0.05, 300.0, 24.0);
// the game's near and far clip (metres) for its reversed-Z depth buffer (Unreal: no far plane, a huge far stands in)
uniform float3 HostPlanes = float3(0.1, 1.0e7, 0.0);
// her rectangle of the whole picture in uv (x, y, width, height): the textures hold only that (1: no crop)
uniform float4 AstraRect = float4(0.0, 0.0, 1.0, 1.0);
// rows of R_her^T R_game (game camera ray -> her picture's camera ray), the game's camera in her picture's camera
// space, x = tan(game vertical fov / 2), y = tan(her picture's vertical fov / 2), z = her picture's aspect
uniform float3 AstraWarpRow0 = float3(1.0, 0.0, 0.0);
uniform float3 AstraWarpRow1 = float3(0.0, 1.0, 0.0);
uniform float3 AstraWarpRow2 = float3(0.0, 0.0, 1.0);
uniform float3 AstraWarpT = float3(0.0, 0.0, 0.0);
uniform bool AstraWarpIdentity = false; // her picture is of this very camera: no re-projection
uniform float3 AstraWarpTan = float3(0.42, 0.42, 1.7778);
// her view from the sun, in the game's camera space: centre, right (+u), up (+v), the way the light travels;
// half width, near, N; and how dark her shadow may be (0: no sun on her)
uniform bool SunVActive = false;
uniform float3 SunVCenter = float3(0.0, 0.0, -5.0);
uniform float3 SunVRight = float3(1.0, 0.0, 0.0);
uniform float3 SunVUp = float3(0.0, 1.0, 0.0);
uniform float3 SunVDir = float3(0.0, -1.0, 0.0);
uniform float3 SunVParams = float3(1.0, 0.0, 512.0);
uniform float AstraShadow = 0.0;

uniform float2 DepthUvScale < ui_type = "drag"; ui_min = 0.1; ui_max = 1.0; ui_step = 0.001; ui_label = "Depth buffer part used";
	ui_tooltip = "Under upscaling (TSR, DLSS, FSR) a game may draw its depth into the top-left part of a larger buffer.\n"
				 "If her occlusion is offset or scaled, set this to render size / buffer size."; > = float2(1.0, 1.0);
uniform float DepthBias < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.005; ui_label = "Depth bias (m)";
	ui_tooltip = "How far behind the game's surface she may still show (her feet on the ground)."; > = 0.04;
uniform float SlopeBias < ui_type = "drag"; ui_min = 0.0; ui_max = 40.0; ui_step = 0.1; ui_label = "Depth bias per grazing slope"; > = 0.0;
uniform float MaxBias < ui_type = "drag"; ui_min = 0.1; ui_max = 10.0; ui_step = 0.1; ui_label = "Largest depth bias (m)"; > = 3.0;
uniform float LightMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match the game's lighting";
	ui_tooltip = "Darken/tint her by the game's light around her. She is already lit (MToon) by her engine, so only a little."; > = 0.3;
uniform float LightReference < ui_type = "drag"; ui_min = 0.1; ui_max = 1.0; ui_step = 0.01; ui_label = "Neutral brightness"; > = 0.42;
uniform float LightTint < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Light colour"; > = 0.55;
// The game's picture around her is its surfaces' colour times its light: green grass reads as green light. Two guards
// on that estimate (light_tint); LightTintMax 0.5 with LightSceneCast 0 is the look before them:
uniform float LightTintMax < ui_type = "drag"; ui_min = 0.0; ui_max = 0.5; ui_step = 0.01; ui_label = "Light colour: largest shift";
	ui_tooltip = "How far the light colour may move any of her colour channels from neutral (before \"Match the game's lighting\" scales it).\n"
				 "0.5: no limit but the 0.55-1.5 clamp. Lower keeps a strongly coloured surface around her (grass, sand) from painting her."; > = 0.1;
uniform float LightSceneCast < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Light colour: ignore the scene's own cast";
	ui_tooltip = "The colour the whole picture shares (its mean: mostly its surfaces' own colour, or the game's grade) is not counted as light\n"
				 "where the picture around her shares it: a green floor around her is grass, not green light. A local colour that differs\n"
				 "from it (a red lamp in a green park) stays. Her engine lights her with the game's sun when the provider sends it. 0: off."; > = 1.0;
uniform float GradeMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match the game's colour grade";
	ui_tooltip = "Drawn at presentation only: before the game's tonemapper its own grade colours her."; > = 0.25;
uniform float FogMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01; ui_label = "Match the game's fog";
	ui_tooltip = "Her fog by the game's own: its colour and density measured from its picture every frame (1: as measured)."; > = 1.0;
uniform float ShadowStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Her shadow";
	ui_tooltip = "How dark her shadow on the game's ground is in full sun (needs the game's sun from the provider)."; > = 0.5;
uniform float ReprojectThickness < ui_type = "drag"; ui_min = 0.05; ui_max = 3.0; ui_step = 0.01; ui_label = "Her depth for re-projection (m)";
	ui_tooltip = "When her picture is a frame older than the camera, a point this far behind her front still counts as her."; > = 0.35;
uniform int DebugView < ui_type = "combo"; ui_items = "Composite\0Her depth (1 m bands)\0Game depth (1 m bands)\0"; > = 0;

float src_linear(float d, float3 planes)
{
	const int flags = int(planes.z + 0.5);
	const float n = planes.x, f = planes.y;
	if (flags & 8)
		return d > 0.0 ? d : 1e9;
	if (flags & 4)
		return d > 0.0 ? n * f / (n + d * (f - n)) : 1e9;
	return d < 1.0 ? n * f / (f - d * (f - n)) : 1e9;
}

/// Where a point of her picture is in it: ndc (y up) -> uv of the WHOLE picture, by its row order.
float2 src_uv(float2 ndc, float3 planes)
{
	const bool bottom_up = (int(planes.z + 0.5) & 2) != 0;
	return bottom_up ? ndc * 0.5 + 0.5 : float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

/// The whole picture's uv -> the uploaded rectangle's; false outside it (she is transparent there).
bool rect_uv(float2 uv, out float2 r)
{
	r = (uv - AstraRect.xy) / AstraRect.zw;
	return all(r >= 0.0) && all(r <= 1.0);
}

/// Her depth (metres) at a point of the whole picture; 1e9 where she isn't.
float her_depth(float2 uv)
{
	float2 r;
	if (!rect_uv(uv, r))
		return 1e9;
	return src_linear(tex2Dlod(sAstraDepth, float4(r, 0, 0)).r, AstraPlanes);
}

/// Her colour (premultiplied) at a point of the whole picture; nothing outside her rectangle.
float4 her_colour(float2 uv)
{
	float2 r;
	if (!rect_uv(uv, r))
		return 0.0;
	return tex2Dlod(sAstraWorld, float4(r, 0, 0));
}

float3 warp_point(float3 p)
{
	return float3(dot(AstraWarpRow0, p), dot(AstraWarpRow1, p), dot(AstraWarpRow2, p)) + AstraWarpT;
}

/// Re-projection: march this game pixel's ray (in the game's presented camera) from near to far, moving each point
/// into the camera her picture was drawn with (rotation + how far the camera moved since). The first point behind
/// her surface there is where she is seen: refine it. She is thin and close, so she is found even when the camera
/// swung sideways. Returns her picture's uv (whole picture) and her depth in the game's camera, or false.
bool reproject(float2 uv, float zFar, out float2 suv, out float zs)
{
	suv = 0.0;
	zs = 1e9;
	const float3 planes = AstraPlanes;
	const float3 tanv = AstraWarpTan;
	const float3 ray = float3((uv.x * 2.0 - 1.0) * tanv.x * BUFFER_WIDTH * BUFFER_RCP_HEIGHT, (1.0 - uv.y * 2.0) * tanv.x, -1.0);
	const float2 scale = float2(tanv.y * tanv.z, tanv.y);
	const float zNear = 0.2;
	// her picture was drawn from this very camera: no marching, the pixel itself
	if (AstraWarpIdentity)
	{
		suv = src_uv(ray.xy / scale, planes);
		zs = her_depth(suv);
		return zs < 1e8 && zs < zFar + 0.05;
	}
	float zprev = zNear;
	for (int i = 0; i < AstraLoops.x; ++i)
	{
		const float z = zNear * pow(zFar / zNear, i / (AstraLoops.x - 1.0));
		const float3 pm = warp_point(ray * z);
		if (pm.z > -1e-3)
			continue;
		const float2 n = pm.xy / -pm.z / scale;
		if (any(abs(n) > 1.0))
			continue;
		const float zmv = her_depth(src_uv(n, planes));
		if (zmv > -pm.z + 0.03)
		{
			zprev = z;
			continue; // still in front of whatever she is there
		}
		// the step went from in front of her to behind: find where (halving), then is it her surface or a gap behind?
		float lo = zprev, hi = z;
		for (int b = 0; b < AstraLoops.y; ++b)
		{
			const float mid = 0.5 * (lo + hi);
			const float3 mb = warp_point(ray * mid);
			const float2 nb = mb.xy / -mb.z / scale;
			const float zb = mb.z < -1e-3 && all(abs(nb) <= 1.0) ? her_depth(src_uv(nb, planes)) : 1e9;
			if (zb > -mb.z + 0.03)
				lo = mid;
			else
				hi = mid;
		}
		{
			const float3 mh = warp_point(ray * hi);
			const float2 nh = mh.xy / -mh.z / scale;
			const float zh2 = mh.z < -1e-3 && all(abs(nh) <= 1.0) ? her_depth(src_uv(nh, planes)) : 1e9;
			if (zh2 > 1e8 || -mh.z - zh2 > ReprojectThickness)
			{
				zprev = z;
				continue; // the ray passes behind her (taking it would smear her along the motion)
			}
		}
		// crossed her surface: settle on it (fixed-point on the surface point's depth in the game's camera)
		float zg = hi;
		float2 ndc;
		{
			const float3 mh = warp_point(ray * hi);
			ndc = mh.xy / -mh.z / scale;
		}
		for (int k = 0; k < AstraLoops.z; ++k)
		{
			const float3 mk = warp_point(ray * zg);
			if (mk.z > -1e-3)
				break;
			const float2 nk = mk.xy / -mk.z / scale;
			if (any(abs(nk) > 1.0))
				break;
			const float zk = her_depth(src_uv(nk, planes));
			if (zk > 1e8)
				break;
			ndc = nk;
			const float3 q = float3(nk * scale, -1.0) * zk - AstraWarpT;
			zg = -(q.x * AstraWarpRow0.z + q.y * AstraWarpRow1.z + q.z * AstraWarpRow2.z);
		}
		suv = src_uv(ndc, planes);
		zs = zg;
		return true;
	}
	return false;
}

/// The game's depth -> metres along its view axis (reversed Z; with no far plane, near / depth).
float host_linear(float d)
{
	const float n = HostPlanes.x, f = HostPlanes.y;
	return d > 0.0 ? n * f / (n + d * (f - n)) : 1e9;
}

float game_depth(float2 uv)
{
	return host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv * DepthUvScale, 0, 0)).x);
}

float luma(float3 c)
{
	return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// ---- her picture into the game's HDR scene colour: the add-on's src/tone.h is the reference ----------------------
// Unreal's DEFAULT tonemapping (the film curve, its desaturations, blue correction, the gamut expansion of bright
// saturated colours) stands in for the game's - inverted to put her into its scene, run forward to measure the light
// and fog around her as the game will show them. The add-on sets the matrices below and binds the curve's table
// (ASTRATONE: N x 2, R32F - row 0 the inverse, row 1 the curve) from tone.h each time the effects run before the
// tonemapper, so nothing here can drift from it. Unreal's scene colour is pre-exposed: its tonemapper applies ~1 more.
texture AstraToneTex : ASTRATONE;
sampler sAstraTone { Texture = AstraToneTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
uniform float3 ToneToAp1_0 < hidden = true; > = float3(1.0, 0.0, 0.0);
uniform float3 ToneToAp1_1 < hidden = true; > = float3(0.0, 1.0, 0.0);
uniform float3 ToneToAp1_2 < hidden = true; > = float3(0.0, 0.0, 1.0);
uniform float3 ToneFromAp1_0 < hidden = true; > = float3(1.0, 0.0, 0.0);
uniform float3 ToneFromAp1_1 < hidden = true; > = float3(0.0, 1.0, 0.0);
uniform float3 ToneFromAp1_2 < hidden = true; > = float3(0.0, 0.0, 1.0);
uniform float3 ToneExpand0 < hidden = true; > = float3(1.0, 0.0, 0.0);
uniform float3 ToneExpand1 < hidden = true; > = float3(0.0, 1.0, 0.0);
uniform float3 ToneExpand2 < hidden = true; > = float3(0.0, 0.0, 1.0);
uniform float3 ToneFwdIn0 < hidden = true; > = float3(1.0, 0.0, 0.0);
uniform float3 ToneFwdIn1 < hidden = true; > = float3(0.0, 1.0, 0.0);
uniform float3 ToneFwdIn2 < hidden = true; > = float3(0.0, 0.0, 1.0);
uniform float3 ToneInvOut0 < hidden = true; > = float3(1.0, 0.0, 0.0);
uniform float3 ToneInvOut1 < hidden = true; > = float3(0.0, 1.0, 0.0);
uniform float3 ToneInvOut2 < hidden = true; > = float3(0.0, 0.0, 1.0);
uniform float3 ToneFwdOut0 < hidden = true; > = float3(1.0, 0.0, 0.0);
uniform float3 ToneFwdOut1 < hidden = true; > = float3(0.0, 1.0, 0.0);
uniform float3 ToneFwdOut2 < hidden = true; > = float3(0.0, 0.0, 1.0);
uniform float3 ToneInvIn0 < hidden = true; > = float3(1.0, 0.0, 0.0);
uniform float3 ToneInvIn1 < hidden = true; > = float3(0.0, 1.0, 0.0);
uniform float3 ToneInvIn2 < hidden = true; > = float3(0.0, 0.0, 1.0);
uniform float3 ToneLuma < hidden = true; > = float3(0.2722287, 0.6740818, 0.0536895);
// the table's y max, its log10 range, and ExpandGamut
uniform float4 ToneLut < hidden = true; > = float4(1.035, -8.0, 3.0, 1.0);
static const int ToneUnexpand = 6; // tone::kUnexpand (tests/core_tests.cpp checks they match)

float3 tone_mul(float3 r0, float3 r1, float3 r2, float3 v)
{
	return float3(dot(r0, v), dot(r1, v), dot(r2, v));
}

/// A row of the table at texel position pos, linearly between texels (tone::lut_at).
float tone_lut(float pos, int row)
{
	const float n = float(tex2Dsize(sAstraTone).x);
	pos = clamp(pos, 0.0, n - 1.0);
	const float i = min(floor(pos), n - 2.0);
	return lerp(tex2Dfetch(sAstraTone, int2(i, row)).r, tex2Dfetch(sAstraTone, int2(i + 1.0, row)).r, pos - i);
}

/// The film curve on one working-colour channel (tone::curve_from_lut).
float tone_curve(float x)
{
	const float pos = (log2(max(x, 1e-30)) * 0.30103 - ToneLut.y) / (ToneLut.z - ToneLut.y) * (float(tex2Dsize(sAstraTone).x) - 1.0);
	return x > 0.0 ? tone_lut(pos, 1) : 0.0;
}

/// ...and its inverse (tone::curve_inverse_from_lut).
float tone_curve_inverse(float y)
{
	const float pos = sqrt(saturate(y / ToneLut.x)) * (float(tex2Dsize(sAstraTone).x) - 1.0);
	return y > 0.0 ? exp2(tone_lut(pos, 0) * 3.32192809) : 0.0;
}

/// How far the gamut expansion moves an AP1 colour (tone::expand_amount).
float tone_expand_amount(float3 c)
{
	const float luma = dot(c, ToneLuma);
	const float3 d = c / max(luma, 1e-6) - 1.0;
	return luma > 1e-6 ? (1.0 - exp2(-4.0 * dot(d, d))) * (1.0 - exp2(-4.0 * ToneLut.w * luma * luma)) : 0.0;
}

float3 srgb_decode(float3 c)
{
	c = saturate(c);
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 srgb_encode(float3 c)
{
	c = saturate(c);
	return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

/// A shown colour (sRGB, as her picture holds it) -> the game's scene colour (linear, pre-exposed), times her exposure
/// trim (tone::display_to_scene).
float3 display_to_scene(float3 d)
{
	float3 c = clamp(tone_mul(ToneInvIn0, ToneInvIn1, ToneInvIn2, srgb_decode(d)), 0.0, ToneLut.x);
	c = float3(tone_curve_inverse(c.x), tone_curve_inverse(c.y), tone_curve_inverse(c.z));
	c = tone_mul(ToneInvOut0, ToneInvOut1, ToneInvOut2, c);
	const float3 target = c;
	[unroll] for (int k = 0; k < ToneUnexpand; ++k)
		c = target - tone_expand_amount(c) * (tone_mul(ToneExpand0, ToneExpand1, ToneExpand2, c) - c);
	return max(tone_mul(ToneFromAp1_0, ToneFromAp1_1, ToneFromAp1_2, c), 0.0) * ToneExposure;
}

/// The game's colour where this runs -> about what the game will show there (sRGB): the light, fog and grade around her
/// are measured in her picture's own space wherever the effect runs (tone::scene_to_display). Her exposure trim is
/// hers: it never enters what the world measures.
float3 host_display(float3 c)
{
	if (!AstraHdr)
		return c;
	c = tone_mul(ToneToAp1_0, ToneToAp1_1, ToneToAp1_2, c);
	c += (tone_mul(ToneExpand0, ToneExpand1, ToneExpand2, c) - c) * tone_expand_amount(c);
	c = max(tone_mul(ToneFwdIn0, ToneFwdIn1, ToneFwdIn2, c), 0.0);
	c = float3(tone_curve(c.x), tone_curve(c.y), tone_curve(c.z));
	return srgb_encode(tone_mul(ToneFwdOut0, ToneFwdOut1, ToneFwdOut2, c));
}

float3 bands(float z)
{
	return lerp(float3(0.1, 0.1, 0.1), float3(0.4, 0.85, 1.0), step(0.5, frac(z))) * saturate(1.5 - z / 100.0);
}

/// The game's fog between the camera and a point z metres away: how much of the point still shows (and its colour).
float fog_transmittance(float z, out float3 colour)
{
	const float4 f = tex2Dfetch(sFogParams, int2(0, 0));
	colour = f.yzw;
	return exp(-max(f.x, 0.0) * FogMatch * z);
}

/// The light's colour around her (luma 1, as her multiplier): the picture's colour there (L, of luma lum), less the cast
/// the whole picture (its mean M) shares where the two agree (LightSceneCast), then limited in how far it may move any
/// of her channels from neutral (LightTintMax). At LightSceneCast 0 and LightTintMax 0.5 it is the tint before both.
float3 light_tint(float3 L, float lum, float3 M)
{
	float3 t = L / max(lum, 1e-3);
	if (LightSceneCast > 0.0)
	{
		const float3 m = max(M / max(luma(M), 1e-3), 0.05);
		// the share of the cast here that is the picture's own: along the mean's cast, never past it, never against
		// it (a grey path in a green park stays grey, not magenta)
		const float3 dt = t - 1.0, dm = m - 1.0;
		const float agree = saturate(dot(dt, dm) / max(dot(dm, dm), 1e-4));
		const float3 n = t / m;
		t = lerp(t, n / max(luma(n), 1e-3), agree * LightSceneCast);
	}
	t = clamp(lerp(1.0, t, LightTint), 0.55, 1.5);
	if (LightTintMax < 0.5) // (the clamp keeps every channel within 0.5 of neutral)
	{
		const float3 d = t - 1.0;
		const float shift = max(max(abs(d.x), abs(d.y)), abs(d.z));
		t = 1.0 + d * min(1.0, LightTintMax / max(shift, 1e-4));
	}
	return t;
}

/// Her own shading stays; the game's local light darkens and tints her a little, its grade colours her, its fog too.
float3 relight(float3 c, float2 uv, float z)
{
	const float3 L = tex2Dlod(sAstraLightBlur, float4(uv, 0, 0)).rgb;
	const float lum = luma(L);
	const float gain = clamp(lum / LightReference, 0.42, 1.3);
	const float3 M = tex2Dfetch(sAstraLightMean, int2(0, 0)).rgb;
	c = lerp(c, c * gain * light_tint(L, lum, M), LightMatch);
	const float3 grade = clamp(lerp(1.0, M / max(luma(M), 1e-3), 0.6), 0.6, 1.6);
	c = lerp(c, c * grade, AstraHdr ? 0.0 : GradeMatch); // before the tonemapper the game's own grade colours her
	float3 fog;
	const float t = fog_transmittance(z, fog);
	return lerp(fog, c, t);
}

/// Her shadow: this game surface point in the engine's view of her from the sun; shadowed where it lies further
/// along the light than her surface there (3x3 taps soften the edge).
float her_shadow_map(float3 p)
{
	const float3 d = p - SunVCenter;
	const float2 uv = float2(dot(d, SunVRight), dot(d, SunVUp)) / SunVParams.x;
	if (any(abs(uv) > 1.0))
		return 0.0;
	const float depth = dot(d, SunVDir) + SunVParams.y;
	const float2 tc = float2(uv.x * 0.5 + 0.5, 0.5 - uv.y * 0.5);
	const float texel = 1.0 / SunVParams.z;
	float shadow = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const float z = tex2Dlod(sAstraSun, float4(tc + float2(x, y) * texel * 1.5, 0, 0)).r;
			shadow += z > 0.0 && depth > z + 0.04 ? 1.0 : 0.0;
		}
	return shadow / 9.0;
}

/// One cell: the game's colour and depth there; its dark channel (darkest channel over a small patch, against last
/// frame's fog colour) bounds the fog's transmittance from below, so -ln(1 - dark) / depth bounds its density above.
void PS_FogCells(float4 pos : SV_Position, float2 uv : TEXCOORD, out float outSigma : SV_Target0, out float4 outColour : SV_Target1)
{
	const float4 prev = tex2Dfetch(sFogPrev, int2(0, 0));
	const float3 A = max(prev.yzw, 0.05);
	float dark = 1.0;
	float3 avg = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const float3 c = host_display(tex2Dlod(ReShade::BackBuffer, float4(uv + float2(x, y) * float2(0.33 / FOG_W, 0.33 / FOG_H), 0, 0)).rgb);
			dark = min(dark, min(c.r / A.r, min(c.g / A.g, c.b / A.b)));
			avg += c;
		}
	avg /= 9.0;
	const float d = game_depth(uv);
	// usable: not too near (noise), not the sky (no depth, or the far plane: it is the fog's colour itself)
	const bool sky = d > min(5000.0, HostPlanes.y * 0.9);
	const bool usable = d > 8.0 && d < 2000.0 && !sky;
	outSigma = usable ? -log(max(1.0 - 0.95 * saturate(dark), 0.02)) / d : 1e4;
	// the haziest places show the fog's colour: weight by how washed out they are (the sky counts fully)
	const float w = usable ? pow(saturate(dark), 8.0) : (sky ? 1.0 : 0.0);
	outColour = float4(avg * w, w);
}

/// The frame's fog: density = the mean of the smallest bounds over blocks of cells (the clearest dark detail in each
/// part of the picture), colour = the weighted mean of the haziest; eased from last frame's.
float4 PS_FogParams(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float sum = 0.0, n = 0.0;
	// 8 x 6 blocks of 8 x 6 cells; the sizes come from the texture, so no compiler unrolls these (see AstraLoops)
	const int2 cells = tex2Dsize(sFogSigma), block = cells / int2(FOG_W / 8, FOG_H / 6);
	for (int by = 0; by < cells.y; by += block.y)
		for (int bx = 0; bx < cells.x; bx += block.x)
		{
			float m = 1e4;
			for (int y = 0; y < block.y; ++y)
				for (int x = 0; x < block.x; ++x)
					m = min(m, tex2Dfetch(sFogSigma, int2(bx + x, by + y)).r);
			if (m < 1e3)
			{
				sum += m;
				n += 1.0;
			}
		}
	const float sigma = n > 0.0 ? sum / n : 0.0;
	// the haziest colours' weighted mean (a loop, not mipmaps: see the top)
	float4 c = 0.0;
	for (int cy = 0; cy < cells.y; ++cy)
		for (int cx = 0; cx < cells.x; ++cx)
			c += tex2Dfetch(sFogColour, int2(cx, cy));
	const float4 prev = tex2Dfetch(sFogPrev, int2(0, 0));
	const float3 colour = c.a > 1e-5 ? c.rgb / c.a : (prev.w > 0.0 ? prev.yzw : float3(0.6, 0.6, 0.6));
	const float k = prev.w > 0.0 ? 0.08 : 1.0; // ease over ~a dozen frames
	return float4(lerp(prev.x, sigma, k), lerp(prev.yzw, colour, k));
}

float4 PS_FogKeep(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return tex2Dfetch(sFogParams, int2(0, 0));
}

float4 PS_Light(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(host_display(tex2D(ReShade::BackBuffer, uv).rgb), 1.0);
}

/// The light around her: 8 x 8 cells of the quarter picture per texel (16 bilinear taps of 2 x 2).
float4 PS_LightBlur(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float2 texel = 1.0 / float2(BUFFER_WIDTH / 4, BUFFER_HEIGHT / 4);
	float3 sum = 0.0;
	[unroll] for (int y = 0; y < 4; ++y)
		[unroll] for (int x = 0; x < 4; ++x)
			sum += tex2Dlod(sAstraLight, float4(uv + (float2(x, y) * 2.0 - 3.0) * texel, 0, 0)).rgb;
	return float4(sum / 16.0, 1.0);
}

/// The whole picture's mean (its grade): every texel of the light (a loop sized by the texture: see AstraLoops).
float4 PS_LightMean(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const int2 n = tex2Dsize(sAstraLightBlur);
	float3 sum = 0.0;
	for (int y = 0; y < n.y; ++y)
		for (int x = 0; x < n.x; ++x)
			sum += tex2Dfetch(sAstraLightBlur, int2(x, y)).rgb;
	return float4(sum / max(float(n.x * n.y), 1.0), 1.0);
}

/// What this pass writes where she is not: the game's own colour, its alpha kept (a scene colour may carry one).
float4 PS_Astra(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float4 host4 = tex2D(ReShade::BackBuffer, uv);
	const float3 host = host4.rgb; // the game's linear scene colour (AstraHdr), or its presented picture
	const float zh = game_depth(uv);
	if (DebugView == 2) // the game's depth as ReShade gives it (works without her: check it first)
		return float4(AstraHdr ? display_to_scene(bands(zh)) : bands(zh), host4.a);
	if (!AstraActive)
		return host4;
	const float allow = min(DepthBias + SlopeBias * abs(ddy(zh)), max(MaxBias, DepthBias));
	const float zFar = max(min(zh + allow, 400.0), 0.5);
	float2 auv;
	float za = 1e9;
	float4 astra = 0.0;
	if (reproject(uv, zFar, auv, za))
		astra = her_colour(auv);
	if (DebugView == 1)
	{
		const float3 d = astra.a > 0.0 ? bands(za) : host_display(host) * 0.3;
		return float4(AstraHdr ? display_to_scene(d) : d, host4.a);
	}
	// behind the game's surfaces (and another compositor's, where it drew nearer)
	float visible = za < zh + allow ? 1.0 : 0.0;
#ifdef ASTRA_OCCLUDER_TEX
	if (AstraOccluderActive)
	{
		const float4 o = tex2Dlod(sAstraOccluder, float4(uv, 0, 0));
		if (o.x > 0.5 && o.y > 0.0 && o.y < za - 0.02)
			visible = 0.0;
	}
#endif
	const float cover = astra.a * visible;
	// her shadow on the game's surfaces around her (where she doesn't cover them herself)
	float3 shaded = host;
	if (SunVActive && AstraShadow > 0.0 && ShadowStrength > 0.0 && cover < 0.999 && zh < 200.0)
	{
		const float3 p = float3((uv.x * 2.0 - 1.0) * AstraWarpTan.x * BUFFER_WIDTH * BUFFER_RCP_HEIGHT, (1.0 - uv.y * 2.0) * AstraWarpTan.x, -1.0) * zh;
		float3 fogc;
		const float tg = fog_transmittance(zh, fogc); // the fog over that ground fills her shadow in
		shaded = host * (1.0 - ShadowStrength * AstraShadow * tg * her_shadow_map(p));
	}
	// (her colour is premultiplied: relight it as straight colour, then by her coverage again; before the tonemapper,
	// into the scene's light)
	float3 lit = visible > 0.0 && astra.a > 1e-4 ? relight(astra.rgb / astra.a, uv, za) : 0.0;
	if (AstraHdr)
		lit = display_to_scene(lit);
	float3 c = lit * astra.a * visible + shaded * (1.0 - cover);
	if (AstraHdr)
		return float4(max(c, 0.0), host4.a); // the game's UI comes after, over her
#ifdef ASTRA_GAME_UI
	// the game's UI back over her (the game composited it with its scene before she was drawn over that)
	if (ASTRA_GAME_UI_ACTIVE && cover > 0.001)
	{
		const float4 ui = tex2D(sAstraGameUi, uv);
		const float3 ui_rgb = pow(max(ui.rgb, 0.0), GameUiGamma) * GameUiStrength;
		c = lerp(c, ui_rgb + c * saturate(1.0 - ui.a), saturate(cover));
	}
#endif
	return float4(saturate(c), host4.a);
}

technique AstraPassthrough < enabled = true; ui_tooltip = "Astra, drawn by her engine from the game's camera (on while her engine is linked)."; >
{
	pass FogCells
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_FogCells;
		RenderTarget0 = FogSigmaTex;
		RenderTarget1 = FogColourTex;
	}
	pass FogParams
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_FogParams;
		RenderTarget = FogParamsTex;
	}
	pass FogKeep
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_FogKeep;
		RenderTarget = FogPrevTex;
	}
	pass Light
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Light;
		RenderTarget = AstraLightTex;
	}
	pass LightBlur
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_LightBlur;
		RenderTarget = AstraLightBlurTex;
	}
	pass LightMean
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_LightMean;
		RenderTarget = AstraLightMeanTex;
	}
	pass Astra
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Astra;
	}
}
