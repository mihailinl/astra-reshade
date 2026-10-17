// The device-recreate gap (a game destroys its D3D device and makes a new one in the same process - DS2/Decima
// does this once at start-up, as a normal reload): does the add-on's link to Astra's engine, and the provider
// pipe, come back for the NEW device?
//
// main.cpp's on_present/on_destroy_device glue needs a live ReShade host and a D3D device to drive directly (out
// of reach here - no live game run in this lane), but the restart it performs is exactly one call each:
// pipeserver::stop()+enginelink::stop() when the device that started them is destroyed (on_destroy_device), then
// pipeserver::start()+enginelink::start() again on the next present (on_present's `!g_started`). This test drives
// THAT - stop() then start() again, twice over, standing in for "device A's last present -> destroy_device(A) ->
// init_device(B) -> device B's first present" - against the real link.cpp/pipe.cpp/ws.cpp, with layer.cpp (GPU
// resources; not exercised by the link at all) replaced by tests/layer_stub.cpp.
//
// A Windows program (winsock + named pipes): tools/relinktest.sh builds it with clang-cl and runs it under Wine
// (tests/run.sh), the same way as tests/ws_tests.cpp.
#include "ws.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "layer.h"
#include "link.h"
#include "pipe.h"
#include "provider.h"
#include "wine_test_util.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wine_test;

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

void logf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	std::vprintf(fmt, args);
	va_end(args);
	std::putchar('\n');
}

namespace
{
	bool recv_exact(SOCKET s, void *buf, size_t n, const std::atomic<bool> &quit)
	{
		char *p = static_cast<char *>(buf);
		size_t left = n;
		while (left > 0)
		{
			if (quit)
				return false;
			fd_set set;
			FD_ZERO(&set);
			FD_SET(s, &set);
			timeval slice = {0, 50000};
			const int r = select(0, &set, nullptr, nullptr, &slice);
			if (r < 0)
				return false;
			if (r == 0)
				continue;
			const int got = recv(s, p, int(left), 0);
			if (got <= 0)
				return false;
			p += got;
			left -= size_t(got);
		}
		return true;
	}

	/// A text frame from the client (masked: RFC 6455 requires it client -> server). False: closed or malformed.
	bool recv_text(SOCKET s, std::string &out, const std::atomic<bool> &quit)
	{
		unsigned char head[2];
		if (!recv_exact(s, head, 2, quit))
			return false;
		const int opcode = head[0] & 0x0F;
		if (opcode == 0x8) // close
			return false;
		uint64_t len = head[1] & 0x7F;
		const bool masked = (head[1] & 0x80) != 0;
		if (len == 126)
		{
			unsigned char ext[2];
			if (!recv_exact(s, ext, 2, quit))
				return false;
			len = (uint64_t(ext[0]) << 8) | ext[1];
		}
		else if (len == 127)
		{
			unsigned char ext[8];
			if (!recv_exact(s, ext, 8, quit))
				return false;
			len = 0;
			for (int i = 0; i < 8; ++i)
				len = (len << 8) | ext[i];
		}
		unsigned char mask[4] = {};
		if (masked && !recv_exact(s, mask, 4, quit))
			return false;
		out.assign(size_t(len), '\0');
		if (len > 0 && !recv_exact(s, out.data(), out.size(), quit))
			return false;
		if (masked)
			for (size_t i = 0; i < out.size(); ++i)
				out[i] ^= char(mask[i % 4]);
		return opcode == 0x1 || opcode == 0x0;
	}

	/// An unmasked frame to the client (RFC 6455: server -> client frames are not masked).
	void send_text(SOCKET s, const std::string &text)
	{
		std::vector<char> frame;
		frame.push_back(char(0x81));
		if (text.size() < 126)
			frame.push_back(char(text.size()));
		else
		{
			frame.push_back(char(126));
			frame.push_back(char((text.size() >> 8) & 0xFF));
			frame.push_back(char(text.size() & 0xFF));
		}
		frame.insert(frame.end(), text.begin(), text.end());
		send(s, frame.data(), int(frame.size()), 0);
	}

	/// A stand-in for Astra's engine: completes the WebSocket handshake, answers every hello with its own (naming
	/// a different ring each connection, so a test can tell the SECOND connection's reply from the first's), and
	/// counts connections and hellos separately - proof that a reconnect is a REAL new connection with its own
	/// hello, not the add-on just believing an old one is still good.
	struct FakeEngine
	{
		SOCKET listener = INVALID_SOCKET;
		int port = 0;
		std::atomic<bool> quit{false};
		std::atomic<int> accepted{0};
		std::atomic<int> hellos{0};
		std::thread acceptThread;
		std::vector<std::thread> connThreads;
		std::mutex connLock;

		FakeEngine()
		{
			listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			sockaddr_in a = {};
			a.sin_family = AF_INET;
			inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
			bind(listener, reinterpret_cast<sockaddr *>(&a), sizeof(a));
			int len = sizeof(a);
			getsockname(listener, reinterpret_cast<sockaddr *>(&a), &len);
			port = ntohs(a.sin_port);
			listen(listener, 8);
			acceptThread = std::thread([this] { accept_loop(); });
		}
		~FakeEngine()
		{
			quit = true;
			closesocket(listener);
			if (acceptThread.joinable())
				acceptThread.join();
			std::lock_guard<std::mutex> lock(connLock);
			for (std::thread &t : connThreads)
				if (t.joinable())
					t.join();
		}

		void accept_loop()
		{
			while (!quit)
			{
				fd_set set;
				FD_ZERO(&set);
				FD_SET(listener, &set);
				timeval slice = {0, 50000};
				if (select(0, &set, nullptr, nullptr, &slice) <= 0)
					continue;
				const SOCKET c = accept(listener, nullptr, nullptr);
				if (c == INVALID_SOCKET)
					continue;
				const int generation = ++accepted;
				std::lock_guard<std::mutex> lock(connLock);
				connThreads.emplace_back([this, c, generation] { serve(c, generation); });
			}
		}

		void serve(SOCKET c, int generation)
		{
			std::string req;
			char ch;
			while (req.find("\r\n\r\n") == std::string::npos)
			{
				if (!recv_exact(c, &ch, 1, quit))
				{
					closesocket(c);
					return;
				}
				req += ch;
			}
			const std::string ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
			send(c, ok.data(), int(ok.size()), 0);
			while (!quit)
			{
				std::string m;
				if (!recv_text(c, m, quit))
					break;
				if (m.rfind("{\"t\":\"hello\"", 0) == 0)
				{
					++hellos;
					char reply[128];
					std::snprintf(reply, sizeof(reply), "{\"t\":\"hello\",\"shm\":\"ring-%d\",\"caps\":[]}", generation);
					send_text(c, reply);
				}
			}
			closesocket(c);
		}
	};
}

namespace layer
{
	// test_*: defined in tests/layer_stub.cpp, not declared in layer.h.
	unsigned long long test_map_count();
	std::string test_last_where();
}

/// Device A's present loop starts the link; device A is destroyed (enginelink::stop(), as on_destroy_device
/// does); device B's first present starts it again (enginelink::start(), as on_present's `!g_started` does). If
/// the gap named in the brief is real, the second half never reconnects: no new TCP connection, no second hello,
/// the ring never re-mapped for device B.
static void test_engine_relink()
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	FakeEngine engine;

	// link.cpp's hello is gated on a provider camera having arrived at least once (never claim her for a game
	// nothing can show a picture of - core_tests.cpp's test_link_waits_for_a_provider_camera_before_hello): this
	// test's own in-process push stands in for AstraUE's (or any provider's) first camera line. provider::state()
	// is a process-wide singleton the device recreate never resets, so one push here covers both device A and B.
	provider::Camera seen{};
	seen.fwd[2] = -1.0f;
	seen.up[1] = 1.0f;
	seen.fov_deg = 60.0f;
	seen.near_m = 0.1f;
	provider::state().cameras.push(seen);

	// enginelink::frame() is what a present calls (main.cpp's begin_frame, once per presented frame) - it is the
	// only place a hello is sent. Driven from this (the test's single) thread, exactly as one present thread
	// would drive it for every present of either device - never concurrently with start()/stop() themselves.
	const auto pump_until_answered = [&](int limit_ms) {
		return wait_for(
			[&] {
				enginelink::frame(1920, 1080);
				return enginelink::stats().answered;
			},
			limit_ms);
	};

	// --- device A: on_present's first call for it (g_started false -> true) ---
	enginelink::start(engine.port, "");
	within(4000, "device A links", [&] { CHECK(pump_until_answered(3500)); });
	CHECK(engine.accepted.load() == 1);
	CHECK(engine.hellos.load() == 1);
	CHECK(layer::test_map_count() == 1);
	CHECK(layer::test_last_where() == "ring-1");
	std::printf("relink: device A linked (%d connection(s), %d hello(s), ring mapped at %s)\n", engine.accepted.load(), engine.hellos.load(),
		layer::test_last_where().c_str());

	// --- device A destroyed: on_destroy_device(deviceA) ---
	within(4000, "enginelink::stop() for device A", [] { enginelink::stop(); });
	CHECK(!enginelink::connected());

	// --- device B: on_present's first call for it (g_started false again -> true) ---
	enginelink::start(engine.port, "");
	within(6000, "device B relinks", [&] { CHECK(pump_until_answered(5500)); });
	CHECK(engine.accepted.load() == 2); // a REAL new TCP connection, not the old one believed still good
	CHECK(engine.hellos.load() == 2);   // device B's own hello, not device A's reused
	CHECK(layer::test_map_count() == 2);
	CHECK(layer::test_last_where() == "ring-2"); // the ring was re-mapped for the new connection's reply
	std::printf("relink: device B relinked (%d connection(s), %d hello(s), ring remapped at %s)\n", engine.accepted.load(), engine.hellos.load(),
		layer::test_last_where().c_str());

	within(4000, "enginelink::stop() for device B", [] { enginelink::stop(); });
}

/// The same shape for the provider pipe (\\.\pipe\astra-provider, pipe.cpp): pipeserver::stop() when the device
/// goes, pipeserver::start() again for the next one. A provider's identity line proves end to end that BOTH
/// pipe sessions' bytes actually reached provider::state(), not just that CreateFile succeeded.
static void test_pipe_relink()
{
	const wchar_t *name = L"\\\\.\\pipe\\astra-provider";

	pipeserver::start();
	HANDLE h1 = INVALID_HANDLE_VALUE;
	CHECK(wait_for(
		[&] {
			h1 = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			return h1 != INVALID_HANDLE_VALUE;
		},
		3000));
	CHECK(h1 != INVALID_HANDLE_VALUE);
	{
		const char line[] = "I test1 Game1\n";
		DWORD written = 0;
		WriteFile(h1, line, DWORD(sizeof(line) - 1), &written, nullptr);
	}
	std::string integ, game;
	CHECK(wait_for(
		[&] {
			provider::state().identity(integ, game);
			return integ == "test1";
		},
		2000));
	CHECK(game == "Game1");
	std::printf("relink: pipe session 1 reached the provider (identity %s/%s)\n", integ.c_str(), game.c_str());
	CloseHandle(h1);

	within(4000, "pipeserver::stop() for device A", [] { pipeserver::stop(); });
	CHECK(!pipeserver::stats().connected);

	pipeserver::start();
	HANDLE h2 = INVALID_HANDLE_VALUE;
	const bool reconnected = wait_for(
		[&] {
			h2 = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			return h2 != INVALID_HANDLE_VALUE;
		},
		3000);
	CHECK(reconnected);
	CHECK(h2 != INVALID_HANDLE_VALUE);
	{
		const char line[] = "I test2 Game2\n";
		DWORD written = 0;
		WriteFile(h2, line, DWORD(sizeof(line) - 1), &written, nullptr);
	}
	CHECK(wait_for(
		[&] {
			provider::state().identity(integ, game);
			return integ == "test2";
		},
		2000));
	CHECK(game == "Game2");
	std::printf("relink: pipe session 2 reached the provider (identity %s/%s)\n", integ.c_str(), game.c_str());
	CloseHandle(h2);

	within(4000, "pipeserver::stop() for device B", [] { pipeserver::stop(); });
}

int main()
{
	test_engine_relink();
	test_pipe_relink();
	std::printf("relink: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
