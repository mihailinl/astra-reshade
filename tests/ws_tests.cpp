// The WebSocket client (extern/minecraft-in-death-stranding/ds/src/ws.cpp) against local peers that misbehave - one
// that accepts and never answers, one that answers and then never reads. A Windows program (winsock): tools/wstest.sh
// builds it with clang-cl and runs it under Wine (tests/run.sh). A client that waits without a bound hangs a test: a
// watchdog fails it and ends the process.
#include "ws.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "wine_test_util.h"
#include <atomic>
#include <cstdio>
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

/// A peer on 127.0.0.1 (a free port). `answer`: complete the WebSocket handshake (else never say anything) and send
/// one text message; it never reads after that.
struct Peer
{
	SOCKET listener = INVALID_SOCKET;
	int port = 0;
	bool answer = false;
	std::atomic<int> accepted{0}, gone{0}; // connections accepted; ones the client closed
	std::atomic<bool> quit{false};
	std::vector<SOCKET> held;
	std::thread thread;

	explicit Peer(bool answer_) : answer(answer_)
	{
		listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_port = 0;
		inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
		bind(listener, reinterpret_cast<sockaddr *>(&a), sizeof(a));
		int len = sizeof(a);
		getsockname(listener, reinterpret_cast<sockaddr *>(&a), &len);
		port = ntohs(a.sin_port);
		listen(listener, 16);
		thread = std::thread([this] { run(); });
	}
	~Peer()
	{
		quit = true;
		closesocket(listener);
		thread.join();
		for (SOCKET s : held)
			closesocket(s);
	}
	void run()
	{
		std::vector<SOCKET> silent; // watched for the client closing them
		while (!quit)
		{
			fd_set set;
			FD_ZERO(&set);
			FD_SET(listener, &set);
			for (SOCKET s : silent)
				FD_SET(s, &set);
			timeval slice = {0, 20000};
			if (select(0, &set, nullptr, nullptr, &slice) <= 0)
				continue;
			if (FD_ISSET(listener, &set))
			{
				const SOCKET c = accept(listener, nullptr, nullptr);
				if (c == INVALID_SOCKET)
					continue;
				++accepted;
				held.push_back(c);
				if (!answer)
				{
					silent.push_back(c);
					continue;
				}
				// read the request, answer it, say hello - and never read again
				std::string req;
				char ch;
				while (req.find("\r\n\r\n") == std::string::npos && recv(c, &ch, 1, 0) == 1)
					req += ch;
				const std::string ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
				send(c, ok.data(), int(ok.size()), 0);
				const char frame[] = {char(0x81), 5, 'h', 'e', 'l', 'l', 'o'};
				send(c, frame, int(sizeof(frame)), 0);
			}
			for (size_t i = 0; i < silent.size();)
				if (FD_ISSET(silent[i], &set))
				{
					char buf[4096];
					const int n = recv(silent[i], buf, sizeof(buf), 0);
					if (n <= 0) // the client closed it
					{
						++gone;
						silent.erase(silent.begin() + std::ptrdiff_t(i));
						continue;
					}
					++i;
				}
				else
					++i;
		}
	}
};

int main()
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	// 1. a peer that accepts and never answers: stop() during the handshake returns at once (it waited forever, the
	//    socket out of stop()'s reach)
	{
		Peer peer(false);
		WsClient ws;
		ws.start("127.0.0.1", peer.port);
		CHECK(wait_for([&] { return peer.accepted.load() >= 1; }, 3000));
		Sleep(100); // inside the handshake's wait now
		const auto t0 = Clock::now();
		within(4000, "stop() during a handshake nobody answers", [&] { ws.stop(); });
		const long long took = ms_since(t0);
		std::printf("ws: stop() during an unanswered handshake took %lld ms\n", took);
		CHECK(took < 500 && !ws.connected());
	}
	// 2. ...and not stopped: the handshake is given up on after kHandshakeMs and tried again
	{
		Peer peer(false);
		WsClient ws;
		ws.start("127.0.0.1", peer.port);
		const auto t0 = Clock::now();
		CHECK(wait_for([&] { return peer.gone.load() >= 1; }, WsClient::kHandshakeMs + 2000));
		const long long gave_up = ms_since(t0);
		CHECK(wait_for([&] { return peer.accepted.load() >= 2; }, 3000));
		std::printf("ws: an unanswered handshake given up after %lld ms, tried again\n", gave_up);
		CHECK(gave_up >= WsClient::kHandshakeMs - 500 && !ws.connected());
		within(4000, "stop()", [&] { ws.stop(); });
	}
	// 3. stop() right after start() (before or while it connects), many times: never a hang
	{
		Peer peer(false);
		long long worst = 0;
		for (int i = 0; i < 40; ++i)
		{
			WsClient ws;
			ws.start("127.0.0.1", peer.port);
			if (i % 2)
				Sleep(i % 7);
			const auto t0 = Clock::now();
			within(4000, "stop() right after start()", [&] { ws.stop(); });
			worst = std::max(worst, ms_since(t0));
		}
		std::printf("ws: stop() right after start(), 40 times: worst %lld ms\n", worst);
		CHECK(worst < 500);
	}
	// 4. a peer that answers, then never reads: its message arrives; sends stop blocking (each within kSendMs and a
	//    margin) and the link is dropped instead of hanging the sender (the game's present thread)
	{
		Peer peer(true);
		WsClient ws;
		ws.start("127.0.0.1", peer.port);
		const auto t0 = Clock::now();
		while (!ws.connected() && ms_since(t0) < 3000)
			Sleep(5);
		CHECK(ws.connected());
		std::string m;
		const auto t1 = Clock::now();
		while (!ws.poll(m) && ms_since(t1) < 2000)
			Sleep(5);
		CHECK(m == "hello");
		const std::string big(64 * 1024, 'x');
		long long worst = 0;
		bool refused = false;
		within(20000, "send() to a peer that never reads", [&] {
			for (int i = 0; i < 4000 && !refused; ++i)
			{
				const auto t = Clock::now();
				refused = !ws.send(big);
				worst = std::max(worst, ms_since(t));
			}
		});
		std::printf("ws: sends to a peer that never reads: the slowest took %lld ms, then the link was dropped: %s\n", worst, refused ? "yes" : "no");
		CHECK(refused && worst < WsClient::kSendMs + 1000);
		within(4000, "stop()", [&] { ws.stop(); });
	}
	std::printf("ws: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
