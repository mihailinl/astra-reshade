#include "ring.h"
#include <atomic>
#include <cstring>

namespace ring
{
	namespace
	{
		template <typename T>
		T rd(const uint8_t *p)
		{
			T v;
			std::memcpy(&v, p, sizeof(T));
			return v;
		}

		/// A field the engine changes under us (aligned): a fresh read every time, never cached.
		int64_t volatile_i64(const uint8_t *p)
		{
			return *reinterpret_cast<const volatile int64_t *>(p);
		}
		int32_t volatile_i32(const uint8_t *p)
		{
			return *reinterpret_cast<const volatile int32_t *>(p);
		}
	}

	bool read_header(const uint8_t *view, size_t view_bytes, Header &out)
	{
		if (view == nullptr || view_bytes < size_t(kHeader) || rd<uint32_t>(view) != kMagic)
			return false;
		Header h;
		h.slots = rd<int32_t>(view + 12);
		h.stride = rd<int64_t>(view + 16);
		h.max_w = rd<uint32_t>(view + 24);
		h.max_h = rd<uint32_t>(view + 28);
		if (h.slots < 1 || h.slots > (kHeader - kDescBase) / kDescStride || h.stride <= 0 || h.stride > (int64_t(1) << 32))
			return false;
		out = h;
		return true;
	}

	Result read_latest(const uint8_t *view, const Header &h, int64_t last_publish, Copy copy, Frame &out, std::vector<uint8_t> &colour,
		std::vector<uint8_t> &depth, std::vector<uint8_t> &sun)
	{
		const int64_t published = volatile_i64(view + 32);
		if (published == last_publish)
			return Result::none;
		const int32_t slot = volatile_i32(view + 40);
		if (slot < 0 || slot >= h.slots)
			return Result::busy;
		const uint8_t *desc = view + kDescBase + kDescStride * slot;
		const int64_t seq = volatile_i64(desc);
		if (seq & 1)
			return Result::busy;
		std::atomic_thread_fence(std::memory_order_acquire);
		uint8_t d[kDescStride];
		std::memcpy(d, desc, sizeof(d));

		Frame f;
		f.publish = published;
		f.host_frame = rd<uint64_t>(d + 16);
		f.w = rd<uint32_t>(d + 24);
		f.h = rd<uint32_t>(d + 28);
		f.near_m = rd<float>(d + 32);
		f.far_m = rd<float>(d + 36);
		f.vfov_deg = rd<float>(d + 40);
		f.flags = rd<uint32_t>(d + 44);
		if (f.w == 0 || f.h == 0 || f.w > 16384 || f.h > 16384)
			return Result::bad;
		if (f.flags & kFlagCrop)
		{
			f.rx = rd<uint16_t>(d + 116);
			f.ry = rd<uint16_t>(d + 118);
			f.rw = rd<uint16_t>(d + 120);
			f.rh = rd<uint16_t>(d + 122);
			if (f.rw == 0 || f.rh == 0)
				f.empty = true; // nothing of her in this view
			else if (f.rx + f.rw > f.w || f.ry + f.rh > f.h)
				return Result::bad;
		}
		else
		{
			f.rw = f.w;
			f.rh = f.h;
		}

		const uint8_t *base = view + kHeader + h.stride * slot;
		const size_t plane = size_t(f.rw) * f.rh * 4;
		if (!f.empty)
		{
			if (2 * plane > size_t(h.stride))
				return Result::bad;
			colour.resize(plane);
			depth.resize(plane);
			if (!copy(colour.data(), base, plane) || !copy(depth.data(), base + plane, plane))
				return Result::fault;
			// her view from the sun (plane 3), when the frame carries it
			if ((f.flags & kFlagSunView) && !(f.flags & kFlagNoOverlay))
			{
				const uint8_t *p3 = base + 2 * plane;
				uint8_t sh[kSunHeader];
				if (2 * plane + kSunHeader <= size_t(h.stride) && copy(sh, p3, sizeof(sh)) && rd<uint32_t>(sh) == kSunMagic)
				{
					const uint32_t n = rd<uint32_t>(sh + 4);
					const size_t bytes = size_t(n) * n * 4;
					if (n >= 16 && n <= 2048 && 2 * plane + kSunHeader + bytes <= size_t(h.stride))
					{
						sun.resize(bytes);
						if (!copy(sun.data(), p3 + kSunHeader, bytes))
							return Result::fault;
						SunView &s = f.sun;
						s.n = n;
						for (int i = 0; i < 3; ++i)
						{
							s.center[i] = rd<double>(sh + 8 + 8 * i);
							s.right[i] = rd<float>(sh + 32 + 4 * i);
							s.up[i] = rd<float>(sh + 44 + 4 * i);
							s.dir[i] = rd<float>(sh + 56 + 4 * i);
						}
						s.half = rd<float>(sh + 68);
						s.near_dist = rd<float>(sh + 72);
						s.valid = s.half > 0.0f;
					}
				}
			}
		}
		std::atomic_thread_fence(std::memory_order_acquire);
		if (volatile_i64(desc) != seq)
			return Result::busy; // rewritten while we copied: the next one
		out = f;
		return Result::ok;
	}

	uint64_t latest_host_frame(const uint8_t *view, const Header &h)
	{
		const int32_t slot = volatile_i32(view + 40);
		if (slot < 0 || slot >= h.slots)
			return 0;
		const uint8_t *desc = view + kDescBase + kDescStride * slot;
		return uint64_t(volatile_i64(desc + 16));
	}
}
