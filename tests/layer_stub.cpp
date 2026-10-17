// A test double for layer.h, for tests/relink_tests.cpp: link.cpp's ring-mapping and compositing hooks, traced
// without the real layer.cpp (GPU resources, a ReShade effect runtime, a D3D device - none of them exist in this
// test). Only the handful of layer:: functions link.cpp calls are defined; what is NOT exercised by the restart
// path under test (install, set_hdr_pass, the tone/frame-ring readers the overlay/effects use) is left undefined -
// the linker never asks for it here.
#include "layer.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace layer
{
	namespace
	{
		std::atomic<bool> g_active{false};
		std::atomic<unsigned long long> g_mapCount{0}, g_letGoCount{0};
		std::mutex g_lock;
		std::string g_lastWhere;
	}

	void set_active(bool active) { g_active = active; }
	bool active() { return g_active; }

	void set_frames_at(const char *where)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_lastWhere = where;
		++g_mapCount;
	}

	void let_go_of_frames() { ++g_letGoCount; }

	void set_host(const view::Pose &, float, float) {}
	void set_shadow(float) {}
	void expect_frame(unsigned long long) {}

	// Test introspection only (not declared in layer.h): how many times the ring was (re)mapped, and at what the
	// link last said it was - proof that a SECOND connection's hello re-maps the ring, not just reuses the first's.
	unsigned long long test_map_count() { return g_mapCount; }
	unsigned long long test_let_go_count() { return g_letGoCount; }
	std::string test_last_where()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		return g_lastWhere;
	}
}
