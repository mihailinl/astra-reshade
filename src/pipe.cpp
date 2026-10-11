#include "pipe.h"
#include "astra_provider.h"
#include "lines.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <thread>

namespace pipeserver
{
	namespace
	{
		constexpr const wchar_t *kName = L"\\\\.\\pipe\\astra-provider";

		std::thread g_thread;
		HANDLE g_stop = nullptr;
		std::atomic<bool> g_serving{false}, g_connected{false};
		std::atomic<unsigned long long> g_lines{0}, g_malformed{0}, g_clients{0};

		/// Each parsed line is the provider call it names.
		struct Calls final : lines::Sink
		{
			void camera(unsigned long long frame, const double pos[3], const float fwd[3], const float up[3], float fov_deg, int fov_horizontal, float aspect,
				float near_m, float far_m) override
			{
				astra_provider_camera2(frame, pos, fwd, up, fov_deg, fov_horizontal, aspect, near_m, far_m);
			}
			void avatar(const double pos[3], const float fwd[3], const float up[3], const float vel[3], float speed, int airborne, float scale) override
			{
				astra_provider_avatar(pos, fwd, up, vel, speed, airborne, scale);
			}
			void anchor(const char *point) override { astra_provider_anchor(point); }
			void param(const char *name, float value) override { astra_provider_param(name, value); }
			void visible(int on) override { astra_provider_visible(on); }
			void light(const float toward[3], const float color[3], float intensity, float visible) override
			{
				astra_provider_light(toward, color, intensity, visible);
			}
			void cue(const char *kind, const char *name) override { astra_provider_cue(kind, name); }
			void identity(const char *integration, const char *game) override
			{
				astra_provider_identity(integration, game);
				logf("provider: %s, %s", integration, game);
			}
			void message(const char *text) override { astra_provider_log(text); }
		};

		/// Waits for an overlapped operation or the stop event. False: stopped or failed.
		bool finish(HANDLE h, OVERLAPPED &ov, DWORD &bytes)
		{
			const HANDLE events[2] = {ov.hEvent, g_stop};
			if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0)
			{
				CancelIoEx(h, &ov);
				GetOverlappedResult(h, &ov, &bytes, TRUE);
				return false;
			}
			return GetOverlappedResult(h, &ov, &bytes, FALSE) != FALSE;
		}

		void serve_one(HANDLE h, HANDLE event)
		{
			OVERLAPPED ov = {};
			ov.hEvent = event;
			DWORD bytes = 0;
			bool connected = ConnectNamedPipe(h, &ov) != FALSE;
			if (!connected)
			{
				const DWORD e = GetLastError();
				if (e == ERROR_PIPE_CONNECTED)
					connected = true;
				else if (e == ERROR_IO_PENDING)
					connected = finish(h, ov, bytes);
			}
			if (!connected)
				return;
			g_connected = true;
			++g_clients;
			logf("pipe: a provider connected");
			Calls calls;
			lines::Splitter splitter;
			char buffer[8192];
			for (;;)
			{
				ResetEvent(event);
				ov = {};
				ov.hEvent = event;
				bytes = 0;
				BOOL ok = ReadFile(h, buffer, sizeof(buffer), nullptr, &ov);
				if (!ok && GetLastError() != ERROR_IO_PENDING)
					break; // the provider closed its end (ERROR_BROKEN_PIPE) or something failed
				if (!finish(h, ov, bytes))
					break;
				if (bytes == 0)
					continue;
				const int bad = splitter.feed(buffer, bytes, calls);
				g_malformed += unsigned(bad);
				for (DWORD i = 0; i < bytes; ++i)
					if (buffer[i] == '\n')
						++g_lines;
				if (bad > 0 && g_malformed <= 5)
					logf("pipe: %d malformed line(s) from the provider", bad);
			}
			g_connected = false;
			logf("pipe: the provider disconnected");
			DisconnectNamedPipe(h);
		}

		void run()
		{
			HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			DWORD last_error = 0;
			while (WaitForSingleObject(g_stop, 0) == WAIT_TIMEOUT)
			{
				// one instance, made by this process only (another game holding the name keeps it)
				HANDLE h = CreateNamedPipeW(kName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
					PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 65536, 0, nullptr);
				if (h == INVALID_HANDLE_VALUE)
				{
					g_serving = false;
					const DWORD e = GetLastError();
					if (e != last_error)
						logf("pipe: cannot serve %ls (error %lu); retrying every 5 s", kName, e);
					last_error = e;
					WaitForSingleObject(g_stop, 5000);
					continue;
				}
				if (!g_serving)
					logf("pipe: serving %ls", kName);
				g_serving = true;
				last_error = 0;
				serve_one(h, event);
				CloseHandle(h);
			}
			g_serving = false;
			CloseHandle(event);
		}
	}

	void start()
	{
		if (g_thread.joinable())
			return;
		g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_thread = std::thread(run);
	}

	void stop()
	{
		if (!g_thread.joinable())
			return;
		SetEvent(g_stop);
		g_thread.join();
		CloseHandle(g_stop);
		g_stop = nullptr;
	}

	Stats stats()
	{
		return {g_serving.load(), g_connected.load(), g_lines.load(), g_malformed.load(), g_clients.load()};
	}
}
