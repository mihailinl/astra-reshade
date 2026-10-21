#pragma once
// The values AstraPassthrough.fx must read IN STEP with the frame it draws - her picture's rectangle and planes, the
// camera re-projection, the game's planes, where she is drawn (scene colour or presented picture), her view from the
// sun - as a small texture (ASTRAFRAME: kTexels x 1, RGBA32F) the add-on copies through the same command list as her
// picture, at every run of the effects. Not uniforms: ReShade keeps ONE constant buffer per effect and rewrites it on
// the CPU at every run, while the GPU reads it when it executes that run. Her pass before Unreal's tonemapper is
// recorded into the game's command list, which the GPU executes after the CPU has already written the next frame's
// values: in Scarlet Skips 577 of 600 frames were drawn with the next frame's uniforms. Her picture of frame N drawn
// with frame N+1's crop rectangle is shifted by however far the rectangle moved - it follows her every frame - so
// with the game's camera standing still she jumped 1.3 px and back on 13 of every 100 frames of her idle (0 with
// this texture). A copy in the command stream executes in order with the draw that reads it.
// Portable: tests/core_tests.cpp checks the effect reads each texel at the index this file packs it at.
#include <cstdint>

namespace frame_meta
{
	constexpr int kTexels = 16; // one row of RGBA32F: 256 bytes, D3D12's row pitch alignment

	// what each texel holds (x, y, z, w); the effect declares the same indices as `static const int AstraFrame<Name>`
	constexpr int kRect = 0;      // her rectangle of the whole picture in uv: x, y, width, height (0 0 1 1: no crop)
	constexpr int kPlanes = 1;    // her picture: near, far, flags (ring.h kFlag*), 1 = she is drawn this frame
	constexpr int kHost = 2;      // the game: near, far, 1 = into its HDR scene colour (before its tonemapper), 1 = no re-projection
	constexpr int kWarp0 = 3;     // rows of R_her^T R_game (xyz)
	constexpr int kWarp1 = 4;
	constexpr int kWarp2 = 5;
	constexpr int kWarpT = 6;     // the game's camera in her picture camera's space (xyz)
	constexpr int kWarpTan = 7;   // tan(game vertical fov / 2), tan(her picture's / 2), her picture's aspect
	constexpr int kSunCenter = 8; // her view from the sun in the game camera's space: centre (xyz), w = her shadow's strength
	constexpr int kSunRight = 9;  // +u (xyz)
	constexpr int kSunUp = 10;    // +v (xyz)
	constexpr int kSunDir = 11;   // the way the light travels (xyz)
	constexpr int kSunParams = 12; // half width, near, N (texels a side), 1 = her sun view is used this frame
	constexpr int kRun = 13;      // the add-on's run of the effects this was written for (x, low 24 bits): for tests
	// her pass before the tonemapper, measured fresh this run: the tonemapper's viewport / ReShade's own depth add-on's
	// pick, per axis - the depth buffer's uv scale, for a game whose depth is on a padded grid too (phase B). (0, 0):
	// not measured this run (the Present path, or no depth add-on bound one) - the effect falls back to its own knob.
	constexpr int kDepthScale = 14;

	struct Values
	{
		float rect[4] = {0, 0, 1, 1};
		float near_m = 0.05f, far_m = 300.0f, flags = 0;
		bool active = false;
		float host_near = 0.1f, host_far = 1.0e7f;
		bool hdr = false, identity = false;
		float warp[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
		float warp_t[3] = {};
		float tan_host = 0.42f, tan_from = 0.42f, aspect = 1.7778f;
		float sun_center[3] = {0, 0, -5}, sun_right[3] = {1, 0, 0}, sun_up[3] = {0, 1, 0}, sun_dir[3] = {0, -1, 0};
		float sun_half = 1.0f, sun_near = 0.0f, sun_n = 512.0f, shadow = 0.0f;
		bool sun = false;
		uint64_t run = 0;
		float depth_scale[2] = {0, 0}; // viewport / depth-buffer size this run (hdr only); (0, 0): not measured
	};

	inline void put(float *out, int texel, float x, float y, float z, float w)
	{
		out[texel * 4 + 0] = x;
		out[texel * 4 + 1] = y;
		out[texel * 4 + 2] = z;
		out[texel * 4 + 3] = w;
	}

	/// Two packings draw the same: every texel but kRun (written for tests; it changes at every run and would make every
	/// run an upload). layer.cpp uploads the texture only when the values changed: at Present on D3D12 ReShade's upload
	/// waits for the GPU, and the values of a frame she is not in stay the same.
	inline bool same_drawing(const float *a, const float *b)
	{
		for (int i = 0; i < kTexels * 4; ++i)
			if (i / 4 != kRun && !(a[i] == b[i]))
				return false;
		return true;
	}

	/// The values into kTexels * 4 floats, as the texture holds them.
	inline void pack(const Values &v, float out[kTexels * 4])
	{
		for (int i = 0; i < kTexels * 4; ++i)
			out[i] = 0.0f;
		put(out, kRect, v.rect[0], v.rect[1], v.rect[2], v.rect[3]);
		put(out, kPlanes, v.near_m, v.far_m, v.flags, v.active ? 1.0f : 0.0f);
		put(out, kHost, v.host_near, v.host_far, v.hdr ? 1.0f : 0.0f, v.identity ? 1.0f : 0.0f);
		for (int r = 0; r < 3; ++r)
			put(out, kWarp0 + r, v.warp[r][0], v.warp[r][1], v.warp[r][2], 0.0f);
		put(out, kWarpT, v.warp_t[0], v.warp_t[1], v.warp_t[2], 0.0f);
		put(out, kWarpTan, v.tan_host, v.tan_from, v.aspect, 0.0f);
		put(out, kSunCenter, v.sun_center[0], v.sun_center[1], v.sun_center[2], v.shadow);
		put(out, kSunRight, v.sun_right[0], v.sun_right[1], v.sun_right[2], 0.0f);
		put(out, kSunUp, v.sun_up[0], v.sun_up[1], v.sun_up[2], 0.0f);
		put(out, kSunDir, v.sun_dir[0], v.sun_dir[1], v.sun_dir[2], 0.0f);
		put(out, kSunParams, v.sun_half, v.sun_near, v.sun_n, v.sun ? 1.0f : 0.0f);
		put(out, kRun, float(v.run & 0xFFFFFF), 0.0f, 0.0f, 0.0f);
		put(out, kDepthScale, v.depth_scale[0], v.depth_scale[1], 0.0f, 0.0f);
	}
}
