#pragma once
// Shared by the Windows/Wine test executables (tests/ws_tests.cpp, tests/relink_tests.cpp): a wait without a
// bound hangs a test under Wine, with no terminal to Ctrl-C from - every blocking check runs under `within()`,
// which ends the process if it does not return in time, and `wait_for()` polls a predicate the same way.
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace wine_test
{
	using Clock = std::chrono::steady_clock;

	inline long long ms_since(Clock::time_point t)
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
	}

	/// Runs `f` with a watchdog: true if it returned within `limit_ms`. A call that never returns fails the run
	/// here (the thread cannot be joined: the process ends) rather than hanging the test.
	template <class F>
	bool within(int limit_ms, const char *what, F &&f)
	{
		std::atomic<bool> done{false};
		std::thread t([&] {
			f();
			done = true;
		});
		const auto t0 = Clock::now();
		while (!done && ms_since(t0) < limit_ms)
			Sleep(5);
		if (!done)
		{
			std::printf("FAIL %s: still waiting after %d ms (a wait without a bound) - giving up\n", what, limit_ms);
			std::fflush(stdout);
			ExitProcess(1);
		}
		t.join();
		return true;
	}

	/// Polls `pred` every 5 ms until it is true, or false once `limit_ms` passes. Calls `pred` exactly once per
	/// try (never a last extra time after the loop): a predicate that DOES something on each call (CreateFileW,
	/// opening a connection) must not be invoked again just to read the result it already had.
	template <class Pred>
	bool wait_for(Pred pred, int limit_ms)
	{
		const auto t0 = Clock::now();
		bool ok = pred();
		while (!ok && ms_since(t0) < limit_ms)
		{
			Sleep(5);
			ok = pred();
		}
		return ok;
	}
}
