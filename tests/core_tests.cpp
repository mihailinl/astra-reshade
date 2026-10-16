// Native tests of the add-on's portable core: the pipe's line protocol, the provider's seqlocked state, the
// camera math and the frame-ring reader (crop included). Run: tests/run.sh (any Linux/macOS clang++ or g++).
#include "config_rules.h"
#include "lines.h"
#include "outbox.h"
#include "passfind.h"
#include "pretone_rules.h"
#include "provider_core.h"
#include "ring.h"
#include "tone.h"
#include "view.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
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
	float fwd[3] = {}, up[3] = {}, vel[3] = {}, fov = 0, aspect = 0, near_m = 0, far_m = -1, speed = 0, scale = 0, value = 0;
	unsigned long long frame = 0;
	int horizontal = -1, airborne = -1;
	std::string a, b;
	void camera(unsigned long long f, const double p[3], const float fw[3], const float u[3], float fv, int h, float asp, float n, float fa) override
	{
		calls.push_back("camera");
		far_m = fa;
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
	void anchor(const char *p) override
	{
		calls.push_back("anchor");
		a = p;
	}
	void param(const char *n, float v) override
	{
		calls.push_back("param");
		a = n;
		value = v;
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
	void engine(const char *e) override
	{
		calls.push_back("engine");
		a = e;
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
		r.horizontal == 1 && near(r.aspect, 1.7778) && near(r.near_m, 0.1) && r.far_m == 0.0f); // no far: an infinite projection
	CHECK(parse(r, "C 42 0 0 0 0 0 -1 0 1 0 45 v 0 0.2 4000"));
	CHECK(r.calls.back() == "camera" && near(r.near_m, 0.2) && near(r.far_m, 4000));
	CHECK(!parse(r, "C 42 0 0 0 0 0 -1 0 1 0 45 v 0 0.2 4000 1")); // a field too many
	CHECK(!parse(r, "C 42 0 0 0 0 0 -1 0 1 0 45 v 0 0.2 far"));
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
	CHECK(parse(r, "N hips") && r.calls.back() == "anchor" && r.a == "hips");
	CHECK(!parse(r, "N") && !parse(r, "N hips feet"));
	CHECK(parse(r, "K seated 1") && r.calls.back() == "param" && r.a == "seated" && near(r.value, 1));
	CHECK(parse(r, "K vspeed -2.5") && near(r.value, -2.5));
	CHECK(parse(r, "K vspeed null") && r.value != r.value); // NaN: hand it back to the set's default
	CHECK(!parse(r, "K vspeed") && !parse(r, "K vspeed fast") && !parse(r, "K vspeed inf"));
	CHECK(parse(r, "V 0") && r.calls.back() == "visible0");
	CHECK(parse(r, "V 1") && r.calls.back() == "visible1");
	CHECK(parse(r, "L 0 1 0 1 0.9 0.8 1.0 0.5") && r.calls.back() == "light" && near(r.fwd[0], 0) && near(r.vel[0], 1) && near(r.scale, 0.5));
	CHECK(parse(r, "P seat") && r.a == "play" && r.b == "seat");
	CHECK(parse(r, "T unseat") && r.a == "trigger" && r.b == "unseat");
	CHECK(parse(r, "R") && r.a == "resume");
	CHECK(!parse(r, "P"));
	CHECK(parse(r, "I astra.unreal Scarlet Skips") && r.a == "astra.unreal" && r.b == "Scarlet Skips");
	CHECK(parse(r, "I astra.unreal Scarlet Skips\r") && r.b == "Scarlet Skips");
	CHECK(parse(r, "E unreal") && r.calls.back() == "engine" && r.a == "unreal");
	CHECK(!parse(r, "E") && !parse(r, "E unreal 5"));
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
	// 200000 tries, and on until one read lands (a loaded machine can starve the reader that long)
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (int i = 0; i < 200000 || (reads == 0 && std::chrono::steady_clock::now() < until); ++i)
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
	// her anchor and the provider's own parameters: sticky, validated, a handed-back one sent once as null
	CHECK(st.anchor() == "feet");
	CHECK(st.set_anchor("hips") && st.anchor() == "hips");
	CHECK(!st.set_anchor("hi\"ps") && !st.set_anchor("9lives") && st.anchor() == "hips");
	CHECK(st.set_anchor(nullptr) && st.anchor() == "feet");
	// the game's engine, as declared: a name or nothing, kept until changed
	CHECK(st.engine().empty() && st.set_engine("unreal") && st.engine() == "unreal");
	CHECK(!st.set_engine("un\"real") && st.engine() == "unreal" && st.set_engine("") && st.engine().empty());
	CHECK(provider::valid_ident("seated") && provider::valid_ident("_x9") && !provider::valid_ident("a-b") && !provider::valid_ident(std::string(33, 'a').c_str()));
	CHECK(st.set_param("seated", 1.0f) && st.set_param("vspeed", -2.0f) && st.set_param("vspeed", -3.0f));
	CHECK(!st.set_param("speed", 1.0f) && !st.set_param("airborne", 0.0f) && !st.set_param("bad name", 1.0f) && !st.set_param("x", INFINITY));
	auto ps = st.take_params();
	CHECK(ps.size() == 2 && ps[0].name == "seated" && ps[1].name == "vspeed" && ps[1].value == -3.0f && !ps[1].clear);
	CHECK(st.take_params().size() == 2); // sticky: sent with every placement
	CHECK(st.set_param("seated", NAN));
	ps = st.take_params();
	CHECK(ps.size() == 2 && ps[0].name == "seated" && ps[0].clear);
	ps = st.take_params();
	CHECK(ps.size() == 1 && ps[0].name == "vspeed"); // handed back once, then forgotten
	CHECK(st.set_param("never_set", NAN) && st.take_params().size() == 1);
	for (int i = 0; i < 20; ++i)
		st.set_param(("p" + std::to_string(i)).c_str(), float(i));
	CHECK(st.take_params().size() == provider::kMaxParams);
	CHECK(st.refused == 2 + 2 + 1 + 4 + (20 - (provider::kMaxParams - 1))); // cues, anchors, an engine, params, the 17th on
	// the present callback: one at a time, the last registration wins, NULL unregisters
	static int called = 0;
	struct Cb
	{
		static void one(void *c) { called += *static_cast<int *>(c); }
		static void two(void *) { called += 100; }
	};
	int ctx = 1;
	CHECK(st.present_callback().fn == nullptr);
	st.set_present_callback(&Cb::one, &ctx);
	provider::PresentCallback pc = st.present_callback();
	pc.fn(pc.context);
	CHECK(called == 1);
	st.set_present_callback(&Cb::two, nullptr);
	CHECK(st.present_callback().fn == &Cb::two);
	st.set_present_callback(nullptr, &ctx);
	CHECK(st.present_callback().fn == nullptr && st.present_callback().context == nullptr);
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


// ---- passfind: the tonemapper among a game's draws -------------------------------------------------------------

namespace
{
	constexpr uint32_t kOutW = 1920, kOutH = 1080;
	constexpr uint32_t kRGBA16F = 10, kR11G11B10F = 26, kRGB10A2 = 24, kBGRA8 = 90, kRGBA8 = 27, kR16F = 54, kRG16 = 35, kBC3 = 76, kR32F = 41;

	passfind::Tex tex(uint64_t id, uint32_t w, uint32_t h, uint32_t dxgi, bool written = false)
	{
		return passfind::Tex{id, w, h, passfind::kind_of(dxgi), written};
	}

	/// A draw as recorded: its targets, its viewport, its vertices and what it reads.
	struct Draw
	{
		passfind::Tracker t;
		uint32_t vertices = 3, instances = 1;
		std::vector<passfind::Tex> reads;
		Draw(const passfind::Tex &target, uint32_t targets = 1)
		{
			t.targets(targets, target);
			t.viewport(float(target.w), float(target.h));
		}
		/// The scene colour it reads, if it is the tonemapper (id 0: it is not).
		uint64_t scene(uint32_t out_w = kOutW, uint32_t out_h = kOutH, bool *ambiguous = nullptr) const
		{
			if (!passfind::tonemapper_shape(t, vertices, instances, out_w, out_h))
				return 0;
			return passfind::scene_read(t, reads.data(), reads.size(), ambiguous).id;
		}
	};
}

static void test_passfind()
{
	CHECK(passfind::kind_of(kRGBA16F) == passfind::Kind::hdr && passfind::kind_of(kR11G11B10F) == passfind::Kind::hdr);
	CHECK(passfind::kind_of(9) == passfind::Kind::hdr && passfind::typed_hdr(9) == kRGBA16F && passfind::typed_hdr(kR11G11B10F) == kR11G11B10F);
	CHECK(passfind::kind_of(kRGB10A2) == passfind::Kind::ldr && passfind::kind_of(kBGRA8) == passfind::Kind::ldr && passfind::kind_of(29) == passfind::Kind::ldr);
	CHECK(passfind::kind_of(40) == passfind::Kind::other && passfind::kind_of(kRG16) == passfind::Kind::other && passfind::kind_of(kBC3) == passfind::Kind::other);

	// Recorded in Scarlet Skips (Unreal 5.6, D3D12, 2026-10-14): the frame's draws into targets of the presented size.
	const passfind::Tex backbuffer = tex(10, 1920, 1080, kRGB10A2), scene = tex(1, 1920, 1080, kR11G11B10F), bloom = tex(2, 960, 540, kR11G11B10F),
						exposure = tex(3, 60, 34, kR16F), white = tex(4, 1, 1, kRGBA8);
	// the tonemapper: three vertices into the back buffer (and a second target), reading the scene colour, bloom,
	// a 60x34 exposure texture and a 1x1 one
	{
		Draw d(backbuffer, 2);
		d.reads = {white, exposure, bloom, scene};
		bool ambiguous = true;
		CHECK(d.scene(kOutW, kOutH, &ambiguous) == scene.id && !ambiguous);
	}
	// a G-buffer pass of the same shape (three vertices into an 8-bit target) reads no HDR picture: not it
	{
		Draw d(tex(11, 1920, 1080, kBGRA8));
		d.reads = {tex(12, 1920, 1080, kRG16)};
		CHECK(d.scene() == 0);
	}
	// Slate's UI quads over it: six vertices into the back buffer, reading UI textures (and a 1280x720 image)
	{
		Draw a(backbuffer), b(backbuffer);
		a.vertices = b.vertices = 6;
		a.reads = {white};
		b.reads = {tex(13, 1280, 720, kBC3)};
		CHECK(a.scene() == 0 && b.scene() == 0);
	}
	// the deferred lighting into the HDR scene colour itself, and bloom at half size: not 8/10-bit targets
	{
		Draw a(scene), b(tex(14, 960, 540, kR11G11B10F));
		a.reads = {tex(15, 1920, 1080, kBGRA8), tex(16, 1920, 1080, kRGB10A2)};
		b.reads = {bloom};
		CHECK(a.scene() == 0 && b.scene() == 0);
	}
	// a mesh, instancing, a viewport inside a larger target: not full-screen
	{
		Draw a(backbuffer), b(backbuffer), c(backbuffer);
		a.vertices = 36;
		b.instances = 2;
		c.t.viewport(1280, 720);
		a.reads = b.reads = c.reads = {scene};
		CHECK(a.scene() == 0 && b.scene() == 0 && c.scene() == 0);
	}
	// a scene capture (a mirror, a minimap) tonemapped into a small target is not the presented picture
	{
		Draw d(tex(17, 512, 512, kBGRA8));
		d.reads = {tex(18, 512, 512, kRGBA16F)};
		CHECK(d.scene() == 0);
	}
	// a game that tonemaps at its render resolution (same aspect, at least half size) and upscales after: shown in a
	// trace, never drawn into - each size would be another copy and another effect permutation, and a size following
	// dynamic resolution would free a copy the GPU still reads (she is drawn at Present there); not another aspect
	{
		Draw a(tex(19, 1280, 720, kBGRA8)), b(tex(20, 1280, 1024, kBGRA8)), c(tex(23, 1920, 1080, kBGRA8));
		a.reads = {tex(21, 1280, 720, kRGBA16F)};
		b.reads = {tex(22, 1280, 1024, kRGBA16F)};
		c.reads = {tex(24, 1920, 1080, kRGBA16F)};
		CHECK(a.scene() == 0 && b.scene() == 0 && c.scene() == 24);
		CHECK(passfind::near_presented_size(1280, 720, kOutW, kOutH) && !passfind::near_presented_size(1280, 1024, kOutW, kOutH));
		CHECK(!passfind::presented_size(1440, 810, kOutW, kOutH) && passfind::presented_size(kOutW, kOutH, kOutW, kOutH));
	}
	// an HDR picture of another size, or one bound for writing, is not its input; two of them are flagged
	{
		Draw a(backbuffer), b(backbuffer), c(backbuffer);
		a.reads = {tex(23, 1280, 720, kRGBA16F)};
		b.reads = {tex(24, 1920, 1080, kRGBA16F, true)};
		c.reads = {scene, tex(25, 1920, 1080, kRGBA16F)};
		bool ambiguous = false;
		CHECK(a.scene() == 0 && b.scene() == 0 && c.scene(kOutW, kOutH, &ambiguous) == scene.id && ambiguous);
	}
	// inside an API render pass nothing can be copied; with no Present seen yet nothing is known
	{
		Draw a(backbuffer), b(backbuffer);
		a.t.in_pass = true;
		a.reads = b.reads = {scene};
		CHECK(a.scene() == 0 && b.scene(0, 0) == 0);
	}
	// a compute tonemapper writes ONE 8/10-bit picture of the presented size from an HDR one; Unreal's compute passes
	// recorded beside it in Scarlet Skips do not qualify (TSR writes several, lighting writes HDR, a pass writing an
	// 8-bit picture and a float one is not a tonemapper)
	{
		const passfind::Tex cs[] = {white, bloom, scene, tex(30, 1920, 1080, kRGBA8, true)};
		const passfind::Tex tsr[] = {tex(31, 1920, 1080, kRGBA8), tex(32, 1920, 1080, kRGBA16F), scene, tex(33, 1920, 1080, kR11G11B10F, true),
			tex(34, 1920, 1080, kRGBA8, true), tex(35, 1920, 1080, kRGBA8, true)};
		const passfind::Tex light[] = {tex(36, 1920, 1080, kBGRA8), tex(37, 1920, 1080, kRGB10A2), tex(38, 1920, 1080, kR11G11B10F, true)};
		const passfind::Tex two[] = {tex(39, 1920, 1080, kBGRA8), tex(40, 1920, 1080, kRGB10A2, true), tex(41, 1920, 1080, kR32F, true), scene};
		CHECK(passfind::compute_scene(cs, 4, kOutW, kOutH).id == scene.id);
		CHECK(passfind::compute_scene(tsr, 6, kOutW, kOutH).id == 0 && passfind::compute_scene(light, 3, kOutW, kOutH).id == 0);
		CHECK(passfind::compute_scene(two, 4, kOutW, kOutH).id == 0);
	}
}

// ---- pretone's rules: where she is drawn, the fallback, what is freed when, what a draw reads ------------------------

static void test_pretone_rules()
{
	using pretone_rules::Facts;
	using pretone_rules::Where;
	// before the tonemapper ONLY in a game declared Unreal (by the provider or the launcher's ini): Death Stranding's
	// provider declares nothing (and reads its camera at presentation), a Unity game declares unity - both at Present
	Facts f;
	CHECK(pretone_rules::choose(f).where == Where::present);
	f.declared = "unreal";
	CHECK(pretone_rules::choose(f).where == Where::before);
	Facts ds;
	ds.present_callback = true;
	CHECK(pretone_rules::choose(ds).where == Where::present);
	ds.declared = "unreal";
	CHECK(pretone_rules::choose(ds).where == Where::present); // its camera is read at presentation: too late for the tonemapper
	Facts unity;
	unity.declared = "unity";
	CHECK(pretone_rules::choose(unity).where == Where::present);
	Facts ini;
	ini.ini = "unreal";
	CHECK(pretone_rules::choose(ini).where == Where::before);
	ini.path_present = true;
	CHECK(pretone_rules::choose(ini).where == Where::present);
	Facts hdr;
	hdr.declared = "unreal";
	hdr.hdr_output = true;
	CHECK(pretone_rules::choose(hdr).where == Where::present);

	// her pass at the tonemapper that did not draw (ReShade compiling the effects for the scene colour): one frame lost,
	// then the Present path held, longer each time, then given up - never a run of frames without her
	{
		pretone_rules::Fallback fb;
		int lost = 0, tries = 0;
		// the effects compile for 5 s (300 frames): every try before that is a miss
		for (uint64_t frame = 0; frame < 2000; ++frame)
		{
			if (fb.holding(frame))
				continue;
			++tries;
			const bool drawn = frame >= 300;
			lost += !drawn;
			fb.result(frame, drawn);
		}
		CHECK(lost == 3 && tries > 1500 && !fb.given_up); // misses at 0, 60, 180 - three lost frames in 5 s - drawn from 420
		pretone_rules::Fallback never;
		lost = 0;
		for (uint64_t frame = 0; frame < 100000; ++frame)
			if (!never.holding(frame))
			{
				++lost;
				never.result(frame, false);
			}
		CHECK(never.given_up && lost == pretone_rules::Fallback::kGiveUp);
	}

	// ReShade's effects switched off by the user: no tries (each would be a miss - 8 gave pretone up for the session);
	// switched on again, her pass is drawn at the tonemapper at once
	{
		pretone_rules::Fallback fb;
		uint64_t first_drawn = 0;
		for (uint64_t frame = 0; frame < 3000 && first_drawn == 0; ++frame)
		{
			const bool effects_on = frame >= 2000;
			if (pretone_rules::should_try(fb, frame, effects_on))
			{
				fb.result(frame, effects_on); // with the effects off nothing draws
				if (effects_on)
					first_drawn = frame;
			}
		}
		CHECK(!fb.given_up && first_drawn == 2000);
	}

	// the upload buffers her pass fills: one pass a frame always finds one, and none is reused (or freed to grow)
	// sooner than the scene colour's copies are freed - kGpuFrames presents after the GPU was given it
	{
		pretone_rules::Staging st;
		std::vector<uint64_t> last(pretone_rules::Staging::kSlots, ~uint64_t(0));
		bool always = true, late = true;
		for (uint64_t frame = 100; frame < 400; ++frame)
		{
			const int i = st.pick(frame);
			always = always && i >= 0;
			if (i < 0)
				continue;
			late = late && (last[i] == ~uint64_t(0) || frame >= last[i] + pretone_rules::Retired<int>::kFrames);
			last[i] = frame;
			st.use(i, frame);
		}
		CHECK(always && late);
	}

	// a copy of the scene colour that is replaced is destroyed only frames later (the GPU may still read it)
	{
		pretone_rules::Retired<int> r;
		std::vector<int> destroyed;
		r.retire(1, 10);
		r.collect(12, [&](int x) { destroyed.push_back(x); });
		CHECK(destroyed.empty());
		r.retire(2, 15);
		r.collect(10 + pretone_rules::Retired<int>::kFrames, [&](int x) { destroyed.push_back(x); });
		CHECK(destroyed.size() == 1 && destroyed[0] == 1 && r.items.size() == 1);
		r.collect(100, [&](int x) { destroyed.push_back(x); });
		CHECK(destroyed.size() == 2 && r.items.empty());
	}

	// what a draw reads: the descriptors copied for it since its list's previous draw - never an older copy, which may
	// name a view of a texture freed since (asking a freed texture its size crashes)
	{
		using pretone_rules::Copy;
		static pretone_rules::CopyRing ring, other; // one per recording thread
		pretone_rules::Mark list;                   // the command list's: set at its reset and at every draw
		list.ring = &ring;
		list.at = ring.next;
		const pretone_rules::Table tables[2] = {{0x100, 64}, {0, 0}};
		pretone_rules::Slot out[64];
		// draw 1: the game copies its two textures (from heap 0x900 slots 7, 8) into slots 64, 65, then draws
		ring.push(Copy{0x100, 64, 2, 0x900, 7});
		size_t n = pretone_rules::resolve(ring, pretone_rules::window(ring, list), tables, 2, out, 64);
		CHECK(n == 2 && out[0].src_off == 7 && out[1].src_off == 8 && out[0].slot == 0);
		list.at = ring.next; // the draw
		// draw 2 reuses the same table without copying (the game's descriptor cache hit): no fresh copy, nothing read
		n = pretone_rules::resolve(ring, pretone_rules::window(ring, list), tables, 2, out, 64);
		CHECK(n == 0);
		// draw 3: a copy into slot 65 only: slot 64's old descriptor is not looked up
		ring.push(Copy{0x100, 65, 1, 0x900, 12});
		n = pretone_rules::resolve(ring, pretone_rules::window(ring, list), tables, 2, out, 64);
		CHECK(n == 1 && out[0].slot == 1 && out[0].src_off == 12);
		list.at = ring.next;
		// a list recorded on another thread than its mark's reads nothing
		other.push(Copy{0x100, 64, 2, 0x900, 7});
		CHECK(pretone_rules::window(other, list) == 0);
		// many copies since: at most kLookBack, the newest win
		for (int i = 0; i < 500; ++i)
			ring.push(Copy{0x100, 64, 1, 0x900, uint32_t(i)});
		CHECK(pretone_rules::window(ring, list) == pretone_rules::kLookBack);
		n = pretone_rules::resolve(ring, pretone_rules::window(ring, list), tables, 2, out, 64);
		CHECK(n == 1 && out[0].src_off == 499);
	}
}

// ---- the link's outbox: a frame never waits for the network ---------------------------------------------------

static void test_outbox()
{
	using clock = std::chrono::steady_clock;
	std::mutex lock;
	std::vector<std::string> got;
	std::atomic<int> generation{1};
	std::atomic<bool> slow{true};
	Outbox box;
	box.start(
		[&](const std::string &m) {
			if (slow)
				std::this_thread::sleep_for(std::chrono::milliseconds(300)); // a peer that takes a while (a send's timeout)
			std::lock_guard<std::mutex> hold(lock);
			got.push_back(m);
			return true;
		},
		[&] { return generation.load(); });
	// three messages from a thread that must not wait (the present thread, a recording thread): queued at once
	const auto t0 = clock::now();
	CHECK(box.push("a", 1) && box.push("b", 1) && box.push("c", 1));
	const auto pushed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
	CHECK(pushed < 20);
	for (int i = 0; i < 300 && box.sent() < 3; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	{
		std::lock_guard<std::mutex> hold(lock);
		CHECK(got.size() == 3 && got[0] == "a" && got[1] == "b" && got[2] == "c"); // in order
	}
	// a message for a connection that is gone is not sent on the next one (a new connection starts with its hello)
	slow = false;
	generation = 2;
	box.push("old", 1);
	box.push("hello", 2);
	for (int i = 0; i < 300 && box.sent() < 4; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	{
		std::lock_guard<std::mutex> hold(lock);
		CHECK(got.size() == 4 && got[3] == "hello" && box.dropped() == 1);
	}
	// a peer that stopped taking them: the queue is bounded, a push still never waits
	slow = true;
	const auto t1 = clock::now();
	int refused = 0;
	for (size_t i = 0; i < Outbox::kMax + 20; ++i)
		refused += !box.push("x", 2);
	CHECK(refused >= 19 && std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t1).count() < 50);
	box.stop(); // after the send in flight
	std::printf("outbox: 3 pushes to a slow peer took %lld ms\n", (long long)pushed);
}

/// The link sends every message through its outbox: the one call of the WebSocket's send is the outbox's sink.
static void test_link_sends_through_the_outbox()
{
	std::ifstream f(std::string(ASTRA_SRC) + "/link.cpp");
	CHECK(f.good());
	std::string line;
	int sends = 0, in_sink = 0;
	while (std::getline(f, line))
		if (line.find("g_ws.send(") != std::string::npos)
		{
			++sends;
			in_sink += line.find("g_outbox.start(") != std::string::npos;
		}
	CHECK(sends == 1 && in_sink == 1);
}

// ---- tone: her picture into the scene's light -----------------------------------------------------------------

/// The largest channel difference.
static double off3(const double a[3], const float b[3])
{
	return std::max({std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1]), std::fabs(a[2] - b[2])});
}

/// Her shown colour through the model's inverse, then through the port of Unreal's whole default tonemapping: what the
/// game will show of her.
static void through_unreal(const float *lut, const tone::Matrices &m, const float d[3], double shown[3], float exposure = 1.0f)
{
	float s[3];
	tone::display_to_scene(lut, m, d, exposure, s);
	const double sd[3] = {s[0], s[1], s[2]};
	tone::ue_reference(sd, shown);
}

static void test_tone()
{
	// Unreal's film curve with its defaults: 0.18 -> 0.18, 0 -> 0, rising to 1 + WhiteClip; the inverse inverts it
	CHECK(near(tone::film_curve(0.18), 0.18, 1e-6) && tone::film_curve(0.0) == 0.0 && near(tone::film_curve(1e4), 1.04, 1e-3));
	bool monotonic = true, inverse = true;
	double prev = -1.0;
	for (int i = 0; i <= 2200; ++i)
	{
		const double x = std::pow(10.0, -8.0 + 11.0 * i / 2200.0), y = tone::film_curve(x);
		monotonic = monotonic && y > prev;
		prev = y;
		inverse = inverse && (y < 1e-9 || y > 1.035 || near(std::log10(tone::film_curve_inverse(y)), std::log10(x), 1e-6));
	}
	CHECK(monotonic && inverse);
	// the matrices: sRGB -> AP1 is ACEScg's (Bradford D65 -> D60), and each inverse is one
	const tone::Matrices m = tone::matrices();
	CHECK(near(m.to_ap1.m[0][0], 0.6131, 1e-3) && near(m.to_ap1.m[1][1], 0.9164, 1e-3) && near(m.to_ap1.m[2][2], 0.8698, 1e-3));
	const tone::M3 one = tone::mul(m.fwd_in, m.inv_out), two = tone::mul(m.fwd_out, m.inv_in);
	bool identity = true;
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			identity = identity && near(one.m[i][j], i == j, 1e-9) && near(two.m[i][j], i == j, 1e-9);
	CHECK(identity);

	std::vector<float> lut(2 * tone::kLutN);
	tone::fill_lut(lut.data());
	// the table's curve is the curve; its inverse the inverse (log10 within 0.003) from the darkest value an 8-bit
	// picture holds (sRGB 1/255: linear 0.0003) to the table's top (kYMax: past it the inverse is clamped)
	bool table = true;
	for (int i = 1; i < 400; ++i)
	{
		const float x = std::pow(10.0f, -4.0f + 6.0f * float(i) / 400.0f);
		const float y = float(tone::film_curve(x));
		table = table && near(tone::curve_from_lut(lut.data(), x), y, 2e-4) &&
				(y < 3e-4f || y > 1.03f || near(std::log10(tone::curve_inverse_from_lut(lut.data(), y)), std::log10(x), 3e-3));
	}
	CHECK(table);

	// Unreal shows her as her engine drew her. Greys: exact (the review's: sRGB 0.50 came out 0.558 with the old
	// curve - Narkowicz's ACES fit, per channel in sRGB)
	bool greys = true;
	for (int i = 1; i <= 20; ++i)
	{
		const float g = float(i) / 20.0f, d[3] = {g, g, g};
		double shown[3];
		through_unreal(lut.data(), m, d, shown);
		greys = greys && off3(shown, d) < 0.004;
	}
	CHECK(greys);
	// skin and hair within about a hundredth, cloth (a saturated red, the review's cyan) within a few: what is left is
	// ACES's red modifier, not inverted (the old curve: skin 1.0 0.885 0.838, cyan 0.459 0.853 0.922)
	const float skin[][3] = {{0.98f, 0.85f, 0.78f}, {0.6f, 0.45f, 0.35f}, {0.35f, 0.2f, 0.15f}, {0.95f, 0.8f, 0.85f}, {0.55f, 0.35f, 0.25f}};
	const float cloth[][3] = {{0.2f, 0.25f, 0.45f}, {0.8f, 0.7f, 0.9f}, {0.9f, 0.3f, 0.3f}, {0.3f, 0.8f, 0.9f}};
	double worst_skin = 0.0, worst_cloth = 0.0;
	for (const auto &d : skin)
	{
		double shown[3];
		through_unreal(lut.data(), m, d, shown);
		worst_skin = std::max(worst_skin, off3(shown, d));
	}
	for (const auto &d : cloth)
	{
		double shown[3];
		through_unreal(lut.data(), m, d, shown);
		worst_cloth = std::max(worst_cloth, off3(shown, d));
	}
	std::printf("tone: through Unreal's default tonemapping, skin and hair off by %.4f at worst, cloth %.4f\n", worst_skin, worst_cloth);
	CHECK(worst_skin < 0.02 && worst_cloth < 0.05);
	// a grid of every colour up to 0.9 and saturation 0.6 (saturated: (max - min) / max): the mean within a hundredth;
	// the worst - bright saturated colours near the curve's top, which the game itself can only show lighter - bounded
	double sum = 0.0, grid_worst = 0.0;
	int n = 0;
	for (int r = 0; r <= 18; ++r)
		for (int g = 0; g <= 18; ++g)
			for (int b = 0; b <= 18; ++b)
			{
				const float d[3] = {r * 0.05f, g * 0.05f, b * 0.05f};
				const float hi = std::max({d[0], d[1], d[2]}), lo = std::min({d[0], d[1], d[2]});
				if (hi <= 0.0f || (hi - lo) / hi > 0.6f + 1e-4f)
					continue;
				double shown[3];
				through_unreal(lut.data(), m, d, shown);
				const double e = off3(shown, d);
				sum += e;
				grid_worst = std::max(grid_worst, e);
				++n;
			}
	std::printf("tone: %d grid colours through Unreal's default tonemapping: mean %.4f, worst %.4f\n", n, sum / n, grid_worst);
	CHECK(sum / n < 0.01 && grid_worst < 0.08);

	// the model both ways (what the effect computes): shown -> scene -> shown is itself over the same grid
	double back = 0.0;
	for (int r = 0; r <= 18; r += 2)
		for (int g = 0; g <= 18; g += 2)
			for (int b = 0; b <= 18; b += 2)
			{
				const float d[3] = {r * 0.05f, g * 0.05f, b * 0.05f};
				const float hi = std::max({d[0], d[1], d[2]}), lo = std::min({d[0], d[1], d[2]});
				if (hi <= 0.0f || (hi - lo) / hi > 0.6f + 1e-4f)
					continue;
				float s[3], again[3];
				tone::display_to_scene(lut.data(), m, d, 1.0f, s);
				tone::scene_to_display(lut.data(), m, s, again);
				const double dd[3] = {d[0], d[1], d[2]};
				back = std::max(back, off3(dd, again));
			}
	CHECK(back < 0.05);

	// her exposure trim scales HER scene colour; the world's light is measured without it (it used to be divided by it,
	// so raising her trim made the world measure darker and the light match cancelled the trim)
	const float d[3] = {0.6f, 0.5f, 0.4f};
	float s1[3], s2[3];
	tone::display_to_scene(lut.data(), m, d, 1.0f, s1);
	tone::display_to_scene(lut.data(), m, d, 2.0f, s2);
	CHECK(near(s2[0], 2.0 * s1[0], 1e-5) && near(s2[1], 2.0 * s1[1], 1e-5) && near(s2[2], 2.0 * s1[2], 1e-5));
	// her whitest white stays finite (the curve's top is cut at kYMax), black stays black
	const float white[3] = {1, 1, 1}, black[3] = {0, 0, 0};
	float sw[3], sb[3];
	tone::display_to_scene(lut.data(), m, white, 1.0f, sw);
	tone::display_to_scene(lut.data(), m, black, 1.0f, sb);
	CHECK(std::isfinite(sw[0]) && sw[1] > 2.0f && sw[1] < 100.0f && sb[0] == 0.0f && sb[1] == 0.0f && sb[2] == 0.0f);
}

/// AstraPassthrough.fx takes tone.h's model from the add-on: it must declare every uniform tone.h sets, the curve's
/// table, the same number of fixed-point steps - and its measure of the world must not use her exposure trim.
static void test_tone_in_effect()
{
	std::ifstream f(ASTRA_FX);
	CHECK(f.good());
	std::stringstream ss;
	ss << f.rdbuf();
	const std::string fx = ss.str();
	tone::Uniform u[tone::kUniforms];
	tone::uniforms(u);
	bool all = true;
	for (const tone::Uniform &x : u)
		all = all && std::regex_search(fx, std::regex(std::string(R"(uniform\s+float3\s+)") + x.name + R"(\b)"));
	CHECK(all);
	CHECK(std::regex_search(fx, std::regex(R"(uniform\s+float4\s+ToneLut\b)")) && std::regex_search(fx, std::regex(R"(texture\s+AstraToneTex\s*:\s*ASTRATONE\s*;)")));
	std::smatch m;
	CHECK(std::regex_search(fx, m, std::regex(R"(static const int ToneUnexpand\s*=\s*([0-9]+)\s*;)")) && std::stoi(m[1].str()) == tone::kUnexpand);
	const size_t at = fx.find("float3 host_display(float3 c)");
	CHECK(at != std::string::npos);
	if (at != std::string::npos)
	{
		const std::string body = fx.substr(at, fx.find("\n}\n", at) - at);
		CHECK(body.find("ToneExposure") == std::string::npos && body.find("tone_curve(") != std::string::npos);
	}
	// no curve constant of its own (the old Narkowicz fit's)
	CHECK(fx.find("ToneA") == std::string::npos && fx.find("TonePre") == std::string::npos);
}

// ---- config_rules: where the add-on is pointed, and ReShade.ini without a shipped one ------------------------------

static void test_config_rules()
{
	using config_rules::option_value;
	const wchar_t *k = L"-AstraConfig";
	// Astra's launcher: two arguments, the path a Windows one (Z:\ under Proton) - after UE4SS's own option
	CHECK(option_value(L"\"C:\\G\\Game-Win64-Shipping.exe\" --ue4ss-path Z:\\p\\ue4ss\\UE4SS.dll -AstraConfig Z:\\p\\astra-reshade.ini", k) ==
		L"Z:\\p\\astra-reshade.ini");
	CHECK(option_value(L"x.exe -AstraConfig Z:\\p\\a.ini -windowed", k) == L"Z:\\p\\a.ini");
	CHECK(option_value(L"x.exe -astraconfig\tZ:\\p\\a.ini", k) == L"Z:\\p\\a.ini"); // letter case, a tab
	// the older single-argument form and both quotings
	CHECK(option_value(L"x.exe -AstraConfig=C:\\p\\a.ini -windowed", k) == L"C:\\p\\a.ini");
	CHECK(option_value(L"x.exe \"-AstraConfig=C:\\a b\\x.ini\" -windowed", k) == L"C:\\a b\\x.ini");
	CHECK(option_value(L"x.exe -AstraConfig=\"C:\\a b\\x.ini\" -windowed", k) == L"C:\\a b\\x.ini");
	CHECK(option_value(L"x.exe -AstraConfig \"C:\\Users\\A B\\x.ini\" -windowed", k) == L"C:\\Users\\A B\\x.ini");
	CHECK(option_value(L"x.exe \"-AstraConfig\" \"C:\\a b\\x.ini\"", k) == L"C:\\a b\\x.ini");
	// a path with blanks whose quotes a launch dropped: up to the .ini that ends a word, never into the next option
	CHECK(option_value(L"x.exe -AstraConfig C:\\Users\\A B\\astra.ini -windowed", k) == L"C:\\Users\\A B\\astra.ini");
	CHECK(option_value(L"x.exe -AstraConfig C:\\Users\\A B\\astra.ini", k) == L"C:\\Users\\A B\\astra.ini");
	CHECK(option_value(L"x.exe -AstraConfig C:\\x.cfg -other C:\\y.ini", k) == L"C:\\x.cfg");
	CHECK(option_value(L"x.exe -AstraConfig C:\\a.ini.bak\\b.ini", k) == L"C:\\a.ini.bak\\b.ini");
	// absent, empty, another option that starts the same, the key inside a value, the first one wins
	CHECK(option_value(L"x.exe -windowed", k).empty());
	CHECK(option_value(L"x.exe -AstraConfig", k).empty());
	CHECK(option_value(L"x.exe -AstraConfig ", k).empty());
	CHECK(option_value(L"x.exe -AstraConfig= -windowed", k).empty());
	CHECK(option_value(L"x.exe -AstraConfig -windowed", k).empty());
	CHECK(option_value(L"x.exe -AstraConfigX C:\\a.ini", k).empty());
	CHECK(option_value(L"x.exe --ue4ss-path C:\\-AstraConfig\\u.dll", k).empty());
	CHECK(option_value(L"x.exe -AstraConfig C:\\1.ini -AstraConfig C:\\2.ini", k) == L"C:\\1.ini");

	// the port and token: the first source present decides BOTH, never a mix
	using config_rules::BridgeSource;
	const auto src = [](const wchar_t *name, bool present, const wchar_t *port, const wchar_t *token) {
		BridgeSource s;
		s.name = name, s.present = present, s.port = port, s.token = token;
		return s;
	};
	config_rules::Bridge b = config_rules::pick_bridge({ src(L"Z:\\p\\a.ini", true, L"25601", L"tok-1"), src(L"environment", true, L"9", L"env"),
		src(L"ReShade.ini [ASTRA]", true, L"7", L"old") });
	CHECK(b.port == 25601 && b.token == "tok-1" && b.source == L"Z:\\p\\a.ini");
	b = config_rules::pick_bridge({ src(L"Z:\\p\\a.ini", true, L" 25602 ", L""), src(L"ReShade.ini [ASTRA]", true, L"7", L"old") });
	CHECK(b.port == 25602 && b.token.empty()); // the launcher's empty token lets no stale one through
	b = config_rules::pick_bridge({ src(L"Z:\\p\\a.ini", false, L"", L""), src(L"environment", false, L"", L""),
		src(L"ReShade.ini [ASTRA]", true, L"25603", L"rs") });
	CHECK(b.port == 25603 && b.token == "rs" && b.source == L"ReShade.ini [ASTRA]");
	b = config_rules::pick_bridge({ src(L"a", false, L"1", L"x") });
	CHECK(b.port == config_rules::kDefaultPort && b.token.empty() && b.source == L"defaults");
	for (const wchar_t *bad : { L"", L"0", L"65536", L"-5", L"80x", L"1234567", L"8 0" })
		CHECK(config_rules::pick_bridge({ src(L"a", true, bad, L"") }).port == config_rules::kDefaultPort);
	CHECK(config_rules::pick_bridge({ src(L"a", true, L"65535", L"") }).port == 65535);
	CHECK(config_rules::clean_token(L" a\"b\\c\x01" L"d\x7f" L"e ") == "abcde");
	CHECK(config_rules::clean_token(std::wstring(400, L'z')).size() == 256);

	// ReShade.ini's lists through its API: elements each ended by '\0'
	const char raw[] = ".\\a\\**\0.\\b\0";
	CHECK((config_rules::split_list(raw, sizeof(raw) - 1) == std::vector<std::string> { ".\\a\\**", ".\\b" }));
	CHECK(config_rules::split_list("", 0).empty());
	CHECK((config_rules::split_list(raw, sizeof(raw) - 1) == config_rules::split_list(config_rules::join_list({ ".\\a\\**", ".\\b" }).data(),
		config_rules::join_list({ ".\\a\\**", ".\\b" }).size())));
	// her shaders' folder: added once, at the end, a person's own paths kept; compared as Windows compares paths
	std::vector<std::string> paths;
	CHECK(config_rules::add_search_path(paths, config_rules::kEffectSearchPath) && paths == std::vector<std::string> { ".\\astra-reshade\\Shaders\\**" });
	CHECK(!config_rules::add_search_path(paths, config_rules::kEffectSearchPath) && paths.size() == 1);
	paths = { ".\\reshade-shaders\\Shaders\\**" };
	CHECK(config_rules::add_search_path(paths, config_rules::kEffectSearchPath) && paths.size() == 2 && paths[0] == ".\\reshade-shaders\\Shaders\\**");
	paths = { "./Astra-ReShade/shaders/**" };
	CHECK(!config_rules::add_search_path(paths, config_rules::kEffectSearchPath));
	// the seeds name what the manual install's ReShade.ini set and ReShade's defaults lack
	bool preset = false, tutorial = false;
	for (const config_rules::Seed &s : config_rules::kSeeds)
	{
		preset |= std::string(s.section) == "GENERAL" && std::string(s.key) == "PresetPath" && std::string(s.value) == ".\\astra-reshade\\AstraPreset.ini";
		tutorial |= std::string(s.section) == "OVERLAY" && std::string(s.key) == "TutorialProgress" && std::string(s.value) == "4";
	}
	CHECK(preset && tutorial);
}

int main(int argc, char **argv)
{
	test_lines();
	test_provider();
	test_view();
	test_ring();
	test_passfind();
	test_pretone_rules();
	test_outbox();
	test_link_sends_through_the_outbox();
	test_tone();
	test_tone_in_effect();
	test_config_rules();
	if (argc > 1)
		test_lua_lines(argv[1]);
	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
