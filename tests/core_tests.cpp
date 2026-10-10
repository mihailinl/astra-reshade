// Native tests of the add-on's portable core: the pipe's line protocol, the provider's seqlocked state, the
// camera math and the frame-ring reader (crop included). Run: tests/run.sh (any Linux/macOS clang++ or g++).
#include "lines.h"
#include "provider_core.h"
#include "ring.h"
#include "view.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_failed = 0, g_checks = 0;
#define CHECK(x)                                                                                                                                     \
	do                                                                                                                                               \
	{                                                                                                                                                \
		++g_checks;                                                                                                                                  \
		if (!(x))                                                                                                                                    \
		{                                                                                                                                            \
			++g_failed;                                                                                                                              \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x);                                                                                 \
		}                                                                                                                                            \
	} while (0)

static bool near(double a, double b, double eps = 1e-4)
{
	return std::fabs(a - b) <= eps;
}

// ---- lines -------------------------------------------------------------------------------------------------

struct Recorder : lines::Sink
{
	std::vector<std::string> calls;
	double pos[3] = {};
	float fwd[3] = {}, up[3] = {}, vel[3] = {}, fov = 0, aspect = 0, near_m = 0, speed = 0, scale = 0;
	unsigned long long frame = 0;
	int horizontal = -1, airborne = -1;
	std::string a, b;
	void camera(unsigned long long f, const double p[3], const float fw[3], const float u[3], float fv, int h, float asp, float n) override
	{
		calls.push_back("camera");
		frame = f;
		std::memcpy(pos, p, sizeof(pos));
		std::memcpy(fwd, fw, sizeof(fwd));
		std::memcpy(up, u, sizeof(up));
		fov = fv;
		horizontal = h;
		aspect = asp;
		near_m = n;
	}
	void avatar(const double p[3], const float fw[3], const float u[3], const float v[3], float s, int air, float sc) override
	{
		calls.push_back("avatar");
		std::memcpy(pos, p, sizeof(pos));
		std::memcpy(fwd, fw, sizeof(fwd));
		std::memcpy(up, u, sizeof(up));
		std::memcpy(vel, v, sizeof(vel));
		speed = s;
		airborne = air;
		scale = sc;
	}
	void visible(int on) override { calls.push_back(on ? "visible1" : "visible0"); }
	void light(const float t[3], const float c[3], float i, float v) override
	{
		calls.push_back("light");
		fwd[0] = t[0];
		vel[0] = c[0];
		speed = i;
		scale = v;
	}
	void cue(const char *k, const char *n) override
	{
		calls.push_back("cue");
		a = k;
		b = n;
	}
	void identity(const char *i, const char *g) override
	{
		calls.push_back("identity");
		a = i;
		b = g;
	}
	void message(const char *t) override
	{
		calls.push_back("message");
		a = t;
	}
};

static bool parse(Recorder &r, const char *line)
{
	return lines::parse(line, std::strlen(line), r);
}

static void test_lines()
{
	Recorder r;
	CHECK(parse(r, "C 42 1000.5 2.25 -3 0 0 -1 0 1 0 90 h 1.7778 0.1"));
	CHECK(r.calls.back() == "camera" && r.frame == 42 && near(r.pos[0], 1000.5) && near(r.pos[2], -3) && near(r.fwd[2], -1) && near(r.fov, 90) &&
		r.horizontal == 1 && near(r.aspect, 1.7778) && near(r.near_m, 0.1));
	CHECK(parse(r, "C 43 0 0 0 0 0 -1 0 1 0 60 v 0 0\r"));
	CHECK(r.horizontal == 0 && r.frame == 43);
	CHECK(!parse(r, "C 44 0 0 0 0 0 -1 0 1 0 60 x 0 0"));    // axis
	CHECK(!parse(r, "C 44 0 0 0 0 0 -1 0 1 0 60 v 0"));      // a field short
	CHECK(!parse(r, "C 44 0 0 nan 0 0 -1 0 1 0 60 v 0 0"));  // not finite
	CHECK(!parse(r, "C 44 0 0 1e999 0 0 -1 0 1 0 60 v 0 0")); // overflow
	CHECK(!parse(r, "C -1x 0 0 0 0 0 -1 0 1 0 60 v 0 0"));
	CHECK(parse(r, "A 1 2 3 0 0 1 0 1 0 0.5 0 0 1.4 1 1.2"));
	CHECK(r.calls.back() == "avatar" && near(r.pos[1], 2) && near(r.vel[0], 0.5) && near(r.speed, 1.4) && r.airborne == 1 && near(r.scale, 1.2));
	CHECK(!parse(r, "A 1 2 3 0 0 1 0 1 0 0.5 0 0 1.4 1"));
	CHECK(parse(r, "V 0") && r.calls.back() == "visible0");
	CHECK(parse(r, "V 1") && r.calls.back() == "visible1");
	CHECK(parse(r, "L 0 1 0 1 0.9 0.8 1.0 0.5") && r.calls.back() == "light" && near(r.fwd[0], 0) && near(r.vel[0], 1) && near(r.scale, 0.5));
	CHECK(parse(r, "P seat") && r.a == "play" && r.b == "seat");
	CHECK(parse(r, "T unseat") && r.a == "trigger" && r.b == "unseat");
	CHECK(parse(r, "R") && r.a == "resume");
	CHECK(!parse(r, "P"));
	CHECK(parse(r, "I astra.unreal Scarlet Skips") && r.a == "astra.unreal" && r.b == "Scarlet Skips");
	CHECK(parse(r, "I astra.unreal Scarlet Skips\r") && r.b == "Scarlet Skips");
	CHECK(parse(r, "M hello there  world") && r.a == "hello there  world");
	CHECK(parse(r, "M") && r.a.empty());
	CHECK(!parse(r, "X 1 2"));
	CHECK(!parse(r, "CC 1"));
	CHECK(!parse(r, ""));

	// the splitter: lines across reads, an overlong line dropped whole
	Recorder s;
	lines::Splitter sp;
	const char *part1 = "V 0\nP se", *part2 = "at\nR\n";
	CHECK(sp.feed(part1, std::strlen(part1), s) == 0);
	CHECK(sp.feed(part2, std::strlen(part2), s) == 0);
	CHECK(s.calls.size() == 3 && s.calls[1] == "cue" && s.calls[2] == "cue" && s.a == "resume");
	std::string longline(2000, 'M');
	longline += "\nV 1\n";
	CHECK(sp.feed(longline.data(), longline.size(), s) == 1);
	CHECK(s.calls.back() == "visible1");
	CHECK(sp.feed("Z\n", 2, s) == 1);
}

// ---- provider core -----------------------------------------------------------------------------------------

static void test_provider()
{
	provider::CameraRing ring;
	provider::Camera c;
	CHECK(!ring.get(0, c));
	for (int i = 0; i < 40; ++i)
	{
		provider::Camera w;
		w.frame = 1000 + i;
		ring.push(w);
	}
	CHECK(ring.count() == 40);
	CHECK(ring.get(0, c) && c.frame == 1039 && c.index == 39);
	CHECK(ring.get(1, c) && c.frame == 1038);
	CHECK(ring.get(3, c) && c.frame == 1036);
	CHECK(ring.get(100, c) && c.frame >= 1040 - provider::CameraRing::kSlots + 2); // clamped to the oldest still held

	// a writer hammering one value never shows the reader half of a write
	struct Pair
	{
		uint64_t a, b[8];
	};
	provider::SeqLocked<Pair> v;
	std::atomic<bool> stop{false};
	std::thread writer([&] {
		for (uint64_t i = 1; !stop; ++i)
		{
			Pair p;
			p.a = i;
			for (auto &x : p.b)
				x = i * 3;
			v.write(p);
		}
	});
	int torn = 0, reads = 0;
	for (int i = 0; i < 200000; ++i)
	{
		Pair p;
		if (v.read(p))
		{
			++reads;
			for (auto x : p.b)
				if (x != p.a * 3)
					++torn;
		}
	}
	stop = true;
	writer.join();
	CHECK(torn == 0);
	CHECK(reads > 0);

	CHECK(provider::valid_name("seat") && provider::valid_name("dance_mid") && provider::valid_name("a.b-c"));
	CHECK(!provider::valid_name("") && !provider::valid_name("se\"at") && !provider::valid_name("a b") && !provider::valid_name(std::string(65, 'a').c_str()));
	CHECK(provider::clean_text("Scarlet \"Skips\"\\\x01 ") == "Scarlet Skips");
	provider::State st;
	st.push_cue("play", "seat");
	st.push_cue("play", "bad\"name");
	st.push_cue("fly", "x");
	st.push_cue("resume", "ignored");
	const auto cues = st.take_cues();
	CHECK(cues.size() == 2 && cues[0].name == "seat" && cues[1].kind == "resume" && cues[1].name.empty());
	CHECK(st.refused == 2);
	const double good[3] = {1, 2, 3}, bad[3] = {1, NAN, 3};
	CHECK(provider::finite3(good) && !provider::finite3(bad));
}

// ---- camera math -------------------------------------------------------------------------------------------

static void apply(const view::Pose &p, const float local[3], double world[3])
{
	for (int i = 0; i < 3; ++i)
		world[i] = p.pos[i] + p.col[0][i] * local[0] + p.col[1][i] * local[1] + p.col[2][i] * local[2];
}

static void test_view()
{
	view::Pose a;
	const double pa[3] = {10, 2, -5};
	const float f[3] = {0.3f, -0.2f, -1}, u[3] = {0.1f, 1, 0};
	CHECK(view::make_pose(pa, f, u, 0.5f, a));
	// orthonormal, right-handed: right x up = back
	float c[3];
	view::cross(a.col[0], a.col[1], c);
	CHECK(near(c[0], a.col[2][0]) && near(c[1], a.col[2][1]) && near(c[2], a.col[2][2]));
	for (int i = 0; i < 3; ++i)
		CHECK(near(a.col[i][0] * a.col[i][0] + a.col[i][1] * a.col[i][1] + a.col[i][2] * a.col[i][2], 1.0));
	// looking along f: -back is f normalised
	const float fl = std::sqrt(0.09f + 0.04f + 1.0f);
	CHECK(near(-a.col[2][0], 0.3 / fl) && near(-a.col[2][2], -1.0 / fl));
	const float parallel[3] = {0, 0, -2};
	view::Pose bad;
	CHECK(!view::make_pose(pa, parallel, parallel, 0.5f, bad));

	// warp: a point seen by the host camera, moved into the picture camera, equals that camera's own view of it
	view::Pose b;
	const double pb[3] = {9.5, 2.1, -4.0};
	const float fb[3] = {0.2f, -0.1f, -1}, ub[3] = {0, 1, 0.05f};
	CHECK(view::make_pose(pb, fb, ub, 0.45f, b));
	const double none[3] = {0, 0, 0};
	const view::Warp w = view::warp(a, b, none);
	CHECK(!w.identity);
	const float local[3] = {0.4f, -0.3f, -6.0f};
	double world[3];
	apply(a, local, world);
	float in_b[3];
	for (int i = 0; i < 3; ++i)
		in_b[i] = w.row[i][0] * local[0] + w.row[i][1] * local[1] + w.row[i][2] * local[2] + w.t[i];
	double back[3];
	apply(b, in_b, back);
	CHECK(near(back[0], world[0], 1e-3) && near(back[1], world[1], 1e-3) && near(back[2], world[2], 1e-3));
	CHECK(view::warp(a, a, none).identity);
	const double moved[3] = {0.5, 0, 0};
	CHECK(!view::warp(a, a, moved).identity);

	// fov: horizontal 90 at 16:9 -> vertical tan = 1 / 1.7778
	CHECK(near(view::tan_half_vertical(90.0f, true, 16.0f / 9.0f), 9.0 / 16.0));
	CHECK(near(view::tan_half_vertical(90.0f, false, 16.0f / 9.0f), 1.0));
}

// ---- the frame ring ----------------------------------------------------------------------------------------

struct FakeRing
{
	std::vector<uint8_t> mem;
	int slots = 6;
	int64_t stride = 0;
	template <typename T>
	void put(size_t at, T v)
	{
		std::memcpy(mem.data() + at, &v, sizeof(T));
	}
	FakeRing(uint32_t maxw, uint32_t maxh)
	{
		stride = int64_t(maxw) * maxh * 4 * 3 + 65536;
		mem.assign(ring::kHeader + size_t(stride) * slots, 0);
		put<uint32_t>(0, ring::kMagic);
		put<uint32_t>(4, 1);
		put<uint32_t>(8, 4096);
		put<uint32_t>(12, uint32_t(slots));
		put<int64_t>(16, stride);
		put<uint32_t>(24, maxw);
		put<uint32_t>(28, maxh);
		put<int32_t>(40, -1);
	}
	/// Publishes a frame into `slot`: a w x h picture; under crop only the rectangle's planes are written.
	void publish(int slot, int64_t counter, uint64_t host, uint32_t w, uint32_t h, bool crop, uint16_t rx, uint16_t ry, uint16_t rw, uint16_t rh)
	{
		const size_t desc = ring::kDescBase + ring::kDescStride * slot;
		const size_t base = ring::kHeader + size_t(stride) * slot;
		const uint32_t pw = crop ? rw : w, ph = crop ? rh : h;
		put<int64_t>(desc, 2 * counter);
		put<uint64_t>(desc + 16, host);
		put<uint32_t>(desc + 24, w);
		put<uint32_t>(desc + 28, h);
		put<float>(desc + 32, 0.05f);
		put<float>(desc + 36, 300.0f);
		put<float>(desc + 40, 50.0f);
		put<uint32_t>(desc + 44, 8 | 16 | (crop ? 128 : 0));
		put<uint16_t>(desc + 116, crop ? rx : 0);
		put<uint16_t>(desc + 118, crop ? ry : 0);
		put<uint16_t>(desc + 120, crop ? rw : 0);
		put<uint16_t>(desc + 122, crop ? rh : 0);
		for (uint32_t i = 0; i < pw * ph; ++i)
		{
			put<uint32_t>(base + 4 * i, 0xFF000000u | i);          // colour: alpha 255, a pixel index
			put<float>(base + size_t(pw) * ph * 4 + 4 * i, 2.5f); // depth
		}
		put<int32_t>(40, slot);
		put<int64_t>(32, counter);
	}
};

static bool plain_copy(void *d, const void *s, size_t n)
{
	std::memcpy(d, s, n);
	return true;
}

static FakeRing *g_tearing = nullptr;
static bool tearing_copy(void *d, const void *s, size_t n)
{
	std::memcpy(d, s, n);
	g_tearing->put<int64_t>(ring::kDescBase, 99 * 2); // the engine rewrote slot 0 while we copied
	return true;
}

static bool faulting_copy(void *, const void *, size_t)
{
	return false;
}

static void test_ring()
{
	FakeRing fr(256, 128);
	ring::Header h;
	CHECK(ring::read_header(fr.mem.data(), fr.mem.size(), h) && h.slots == 6 && h.max_w == 256 && h.bytes() == fr.mem.size());
	std::vector<uint8_t> bad(fr.mem);
	bad[0] = 0;
	CHECK(!ring::read_header(bad.data(), bad.size(), h));
	CHECK(ring::read_header(fr.mem.data(), fr.mem.size(), h));

	ring::Frame f;
	std::vector<uint8_t> colour, depth, sun;
	// nothing published yet
	CHECK(ring::read_latest(fr.mem.data(), h, 0, plain_copy, f, colour, depth, sun) == ring::Result::none);
	CHECK(ring::read_latest(fr.mem.data(), h, -1, plain_copy, f, colour, depth, sun) == ring::Result::busy);

	// a whole picture
	fr.publish(0, 1, 7, 64, 32, false, 0, 0, 0, 0);
	CHECK(ring::read_latest(fr.mem.data(), h, -1, plain_copy, f, colour, depth, sun) == ring::Result::ok);
	CHECK(f.host_frame == 7 && f.w == 64 && f.rw == 64 && f.rh == 32 && !f.empty && colour.size() == 64 * 32 * 4);
	float d;
	std::memcpy(&d, depth.data() + 4 * 100, 4);
	CHECK(near(d, 2.5));
	CHECK(ring::read_latest(fr.mem.data(), h, f.publish, plain_copy, f, colour, depth, sun) == ring::Result::none);
	CHECK(ring::latest_host_frame(fr.mem.data(), h) == 7);

	// cropped: only her rectangle
	fr.publish(1, 2, 8, 256, 128, true, 64, 16, 40, 72);
	CHECK(ring::read_latest(fr.mem.data(), h, 1, plain_copy, f, colour, depth, sun) == ring::Result::ok);
	CHECK(f.host_frame == 8 && f.w == 256 && f.h == 128 && f.rx == 64 && f.ry == 16 && f.rw == 40 && f.rh == 72 && !f.empty);
	CHECK(colour.size() == 40 * 72 * 4 && depth.size() == 40 * 72 * 4);
	uint32_t px;
	std::memcpy(&px, colour.data() + 4 * 41, 4);
	CHECK(px == (0xFF000000u | 41));

	// an empty rectangle: nothing of her
	fr.publish(2, 3, 9, 256, 128, true, 0, 0, 0, 0);
	CHECK(ring::read_latest(fr.mem.data(), h, 2, plain_copy, f, colour, depth, sun) == ring::Result::ok);
	CHECK(f.empty && f.host_frame == 9);

	// a rectangle outside the picture is refused
	fr.publish(3, 4, 10, 256, 128, true, 250, 0, 40, 10);
	CHECK(ring::read_latest(fr.mem.data(), h, 3, plain_copy, f, colour, depth, sun) == ring::Result::bad);

	// being written (odd seq): later
	fr.publish(0, 5, 11, 64, 32, false, 0, 0, 0, 0);
	fr.put<int64_t>(ring::kDescBase, 11);
	CHECK(ring::read_latest(fr.mem.data(), h, 4, plain_copy, f, colour, depth, sun) == ring::Result::busy);
	// rewritten while copied: discarded
	fr.put<int64_t>(ring::kDescBase, 10);
	g_tearing = &fr;
	CHECK(ring::read_latest(fr.mem.data(), h, 4, tearing_copy, f, colour, depth, sun) == ring::Result::busy);
	// the copy faulted
	fr.publish(0, 6, 12, 64, 32, false, 0, 0, 0, 0);
	CHECK(ring::read_latest(fr.mem.data(), h, 5, faulting_copy, f, colour, depth, sun) == ring::Result::fault);
	// a slot index out of range
	fr.put<int32_t>(40, 9);
	fr.put<int64_t>(32, 7);
	CHECK(ring::read_latest(fr.mem.data(), h, 6, plain_copy, f, colour, depth, sun) == ring::Result::busy);

	// her sun view rides in plane 3 (flag 32): read with the rectangle's plane size under crop
	fr.publish(4, 8, 13, 256, 128, true, 10, 10, 32, 32);
	const size_t desc = ring::kDescBase + ring::kDescStride * 4, base = ring::kHeader + size_t(fr.stride) * 4, p3 = base + 2 * 32 * 32 * 4;
	fr.put<uint32_t>(desc + 44, 8 | 32 | 128);
	fr.put<uint32_t>(p3, ring::kSunMagic);
	fr.put<uint32_t>(p3 + 4, 16);
	fr.put<double>(p3 + 8, 1.5);
	fr.put<float>(p3 + 68, 2.0f);
	fr.put<float>(p3 + ring::kSunHeader + 4 * 5, 3.25f);
	CHECK(ring::read_latest(fr.mem.data(), h, 7, plain_copy, f, colour, depth, sun) == ring::Result::ok);
	CHECK(f.sun.valid && f.sun.n == 16 && near(f.sun.center[0], 1.5) && near(f.sun.half, 2.0) && sun.size() == 16 * 16 * 4);
	float sv;
	std::memcpy(&sv, sun.data() + 4 * 5, 4);
	CHECK(near(sv, 3.25));
}

/// Lines the AstraUE Lua mod really wrote (tests/astraue_tests.lua against a stub of UE4SS): every one must parse.
static void test_lua_lines(const char *path)
{
	FILE *f = std::fopen(path, "rb");
	CHECK(f != nullptr);
	if (f == nullptr)
		return;
	Recorder r;
	lines::Splitter sp;
	char buf[4096];
	size_t n;
	int bad = 0;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		bad += sp.feed(buf, n, r);
	std::fclose(f);
	CHECK(bad == 0);
	int cams = 0, avatars = 0;
	for (const std::string &c : r.calls)
	{
		cams += c == "camera";
		avatars += c == "avatar";
	}
	CHECK(cams > 0 && cams == avatars);
	std::printf("AstraUE's lines: %zu calls, %d malformed\n", r.calls.size(), bad);
}

int main(int argc, char **argv)
{
	test_lines();
	test_provider();
	test_view();
	test_ring();
	if (argc > 1)
		test_lua_lines(argv[1]);
	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
