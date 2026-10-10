#pragma once
// The engine's frame ring ("MCPT" v1, BRIDGE.md "The frame ring"): reading its newest frame with the
// seqlock, crop-aware (flag 128: the planes are her RECTANGLE only; an empty rectangle = nothing of her).
// Portable: the caller supplies the mapped view and a copy function (the add-on's contains faults).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ring
{
	constexpr uint32_t kMagic = 0x5450434D; // "MCPT"
	constexpr uint32_t kSunMagic = 0x564E5553; // "SUNV"
	constexpr int kHeader = 4096, kDescBase = 256, kDescStride = 128, kSunHeader = 128;
	constexpr uint32_t kFlagNdc01 = 1, kFlagBottomUp = 2, kFlagReversed = 4, kFlagLinear = 8, kFlagNoOverlay = 16, kFlagSunView = 32,
					   kFlagCaster = 64, kFlagCrop = 128;

	struct Header
	{
		int32_t slots = 0;
		int64_t stride = 0;
		uint32_t max_w = 0, max_h = 0;
		/// Bytes a mapping must cover.
		size_t bytes() const { return size_t(kHeader) + size_t(stride) * size_t(slots); }
	};

	/// The header, if this is a ring (magic, sane slots and stride).
	bool read_header(const uint8_t *view, size_t view_bytes, Header &out);

	struct SunView
	{
		bool valid = false;
		uint32_t n = 0;
		double center[3] = {};
		float right[3] = {}, up[3] = {}, dir[3] = {};
		float half = 1.0f, near_dist = 0.0f;
	};

	struct Frame
	{
		int64_t publish = 0;       // the header's counter this frame was read at
		uint64_t host_frame = 0;   // the cam.id it was drawn for
		uint32_t w = 0, h = 0;     // the whole picture
		float near_m = 0, far_m = 0, vfov_deg = 0;
		uint32_t flags = 0;
		// what the planes hold: the rectangle (x, y, w, h) of the w x h picture; the whole picture without crop
		uint32_t rx = 0, ry = 0, rw = 0, rh = 0;
		bool empty = false;        // nothing of her in this picture: clear her
		SunView sun;
	};

	enum class Result
	{
		none,  // nothing new since `last_publish`
		busy,  // being written, or rewritten while copied: try again later
		bad,   // not a usable frame (sizes out of range)
		fault, // the copy faulted (the ring went away)
		ok
	};

	using Copy = bool (*)(void *dst, const void *src, size_t n);

	/// The newest frame, if its publish counter is not `last_publish`: descriptor into `out`, colour plane
	/// (RGBA8 premultiplied, rw x rh) into `colour`, depth (f32 metres) into `depth`, her sun view's N x N
	/// floats into `sun` (flag 32). Nothing in the outputs is valid unless the result is ok.
	Result read_latest(const uint8_t *view, const Header &h, int64_t last_publish, Copy copy, Frame &out, std::vector<uint8_t> &colour,
		std::vector<uint8_t> &depth, std::vector<uint8_t> &sun);

	/// The cam.id of the newest published frame (for waiting on one); 0 if none.
	uint64_t latest_host_frame(const uint8_t *view, const Header &h);
}
