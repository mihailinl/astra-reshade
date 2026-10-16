#include "link.h"
#include "hello_rules.h"
#include "layer.h"
#include "log.h"
#include "outbox.h"
#include "provider.h"
#include "ws.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace enginelink
{
	namespace
	{
		constexpr const char *kFoundation = "0.2.1";       // this add-on's version, told to the engine
		constexpr double kRebase = 1500.0;                 // move the floating origin when she is this far from it
		constexpr float kFar = 300.0f;                     // she is never far: a short depth range keeps it precise
		constexpr int64_t kStaleMs = 300;                  // a provider silent this long: she is hidden
		constexpr int64_t kLightStaleMs = 2000;
		constexpr ULONGLONG kLightEveryMs = 100;

		WsClient g_ws;
		std::string g_host = "127.0.0.1";
		int g_port = 0;
		hello_rules::Backoff g_backoff; // an engine that refused the hello is asked again later, not at every reconnect
		bool g_parked = false;          // the socket is stopped until g_backoff says
		std::thread g_stopper;          // stops it off the present thread (WsClient::stop joins its thread: up to ~0.1 s)
		std::string g_lastShm;          // the ring the last hello reply named
		ULONGLONG g_helloLogAt = 0;
		unsigned g_hellosUnlogged = 0;
		Outbox g_outbox; // every message goes out through it: a frame never waits for the network
		std::string g_token;
		int g_generation = -1;
		bool g_answered = false; // the engine's hello came for this connection
		bool g_capsLight = false, g_capsCues = false;
		double g_origin[3] = {};
		bool g_originSet = false;
		hello_rules::Asked g_asked; // this connection's hellos: the ring asked for and the ring made
		int g_viewW = 1920, g_viewH = 1080;
		unsigned long long g_camId = 0;
		uint64_t g_sentIndex = ~uint64_t(0); // the provider camera last sent (one picture per game camera)
		ULONGLONG g_nextLight = 0;
		int g_delay = 1;
		float g_size = 1.0f;
		bool g_enabled = true;
		Stats g_stats{};
		std::mutex g_eventLock;
		std::string g_event;
		unsigned g_logged = 0;

		struct History
		{
			unsigned long long id = 0; // cam.id sent
			uint64_t index = 0;        // the provider camera's ring index
			view::Pose pose;           // absolute world
			float aspect = 1.0f;       // the projection's w/h
			double feet[3] = {};       // hers, absolute world, when it was sent
			double origin[3] = {};     // the floating origin then (the engine's world = absolute - origin)
		};
		constexpr unsigned kHistory = 64;
		History g_history[kHistory];
		double g_shownFeet[3] = {}; // hers in the picture of the camera being presented (or the newest sent)
		bool g_shownFeetSet = false;
		std::mutex g_historyLock;

		void sendf(const char *fmt, ...)
		{
			char buffer[1536];
			va_list args;
			va_start(args, fmt);
			const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
			va_end(args);
			if (n > 0 && n < int(sizeof(buffer)))
				g_outbox.push(std::string(buffer, size_t(n)), g_ws.generation());
		}

		/// The game's name when no provider gave one: the exe's, without Unreal's "-Win64-Shipping".
		std::string exe_name()
		{
			wchar_t path[MAX_PATH] = L"";
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			const wchar_t *base = wcsrchr(path, L'\\');
			base = base ? base + 1 : path;
			char name[MAX_PATH];
			WideCharToMultiByte(CP_UTF8, 0, base, -1, name, sizeof(name), nullptr, nullptr);
			std::string s = name;
			for (const char *tail : {".exe", ".EXE", "-Win64-Shipping", "-WinGDK-Shipping", "-Win64-Test"})
				if (s.size() > std::strlen(tail) && s.compare(s.size() - std::strlen(tail), std::string::npos, tail) == 0)
					s.erase(s.size() - std::strlen(tail));
			return provider::clean_text(s.c_str());
		}

		/// `want`: the largest picture this connection will ask for (hello_rules::Asked), unknown before the first present.
		/// False: it could not be queued (the outbox is full: the connection stopped taking messages).
		bool say_hello(hello_rules::Size want)
		{
			std::string integration, game;
			provider::state().identity(integration, game);
			if (game.empty())
				game = exe_name();
			if (integration.empty())
				integration = "astra.reshade";
			const bool queued = g_outbox.push(hello_rules::hello(game, kFoundation, integration, g_token, want), g_ws.generation());
			// at most one line every 10 s: a peer that closes every connection once made it ~22 a second
			const ULONGLONG now = GetTickCount64();
			if (now >= g_helloLogAt)
			{
				logf("link: hello %s (game \"%s\", integration %s, want %dx%d%s; %u more since the last line)", queued ? "sent" : "NOT queued",
					game.c_str(), integration.c_str(), want.w, want.h, g_token.empty() ? "" : ", with token", g_hellosUnlogged);
				g_helloLogAt = now + 10000;
				g_hellosUnlogged = 0;
			}
			else
				++g_hellosUnlogged;
			return queued;
		}

		/// A hello's reply is not coming (lost, or the engine answered with an error): the ring the last reply named is
		/// read again - the engine keeps it when it cannot make a larger one.
		void map_again(const char *why)
		{
			g_asked.unanswered();
			if (!g_lastShm.empty())
				layer::set_frames_at(g_lastShm.c_str());
			logf("link: %s: reading the engine's ring at %s again", why, g_lastShm.c_str());
		}

		/// Her picture outgrew the engine's ring: let go of it FIRST (on Windows a ring a reader still holds cannot grow -
		/// the engine would keep the old one), then ask for the larger one; the reply names it, mapped afresh. A hello
		/// that cannot be queued gives the old ring back at once.
		void hello_again(hello_rules::Size want, int64_t now)
		{
			layer::let_go_of_frames();
			if (say_hello(want))
				g_asked.sent(want, now);
			else
				map_again("the hello asking for a larger ring could not be queued");
		}

		/// The engine refused this connection's hello (an `error` before any reply: its games paused after a crash, a
		/// wrong token). It closes the socket and the client would reconnect at once - so the socket is stopped and
		/// started again when the back-off ends (2 s, doubling to 30 s).
		void refused(const std::string &m)
		{
			g_backoff.refused(provider::now_ms());
			logf("link: the engine refused the hello (%.200s): asking again in %lld s", m.c_str(), (long long)(g_backoff.step_ms / 1000));
			if (g_stopper.joinable())
				g_stopper.join();
			g_parked = true;
			g_stopper = std::thread([] { g_ws.stop(); });
		}

		/// The engine's hello reply: where its frames are (map them again at every hello: a restarted engine's ring
		/// is a new file), and what it accepts.
		void on_hello(const std::string &m)
		{
			g_answered = true;
			const size_t at = m.find("\"shm\":\"");
			if (at != std::string::npos)
			{
				const size_t from = at + 7, to = m.find('"', from);
				if (to != std::string::npos)
				{
					std::string where = m.substr(from, to - from);
					for (size_t i = where.find("\\\\"); i != std::string::npos; i = where.find("\\\\", i + 1))
						where.erase(i, 1); // JSON's "\\" -> "\"
					if (!where.empty())
					{
						layer::set_frames_at(where.c_str());
						g_lastShm = where;
					}
				}
			}
			const size_t caps = m.find("\"caps\":[");
			const std::string list = caps == std::string::npos ? std::string() : m.substr(caps, m.find(']', caps) - caps);
			g_capsLight = list.find("\"light\"") != std::string::npos;
			g_capsCues = list.find("\"cues\"") != std::string::npos;
			const hello_rules::Reply reply = hello_rules::read_reply(m);
			g_asked.answered(reply);
			g_backoff.answered();
			logf("link: the engine's ring is %dx%d (asked %dx%d%s)", reply.ring.w, reply.ring.h, g_asked.want.w, g_asked.want.h,
				reply.takes_want ? "" : "; this engine sizes no ring for a game");
		}

		void receive()
		{
			std::string m;
			while (g_ws.poll(m))
			{
				++g_stats.received;
				{
					std::lock_guard<std::mutex> lock(g_eventLock);
					g_event = m.substr(0, 200);
				}
				const bool hello = m.rfind("{\"t\":\"hello\"", 0) == 0;
				const bool error = m.rfind("{\"t\":\"error\"", 0) == 0;
				if (hello || (error && g_logged < 50) || g_logged < 10)
				{
					++g_logged;
					logf("engine: %.300s", m.c_str());
				}
				if (g_parked)
					continue; // the rest of a refused connection's messages
				if (hello)
					on_hello(m);
				else if (error && !g_answered)
					refused(m);
				else if (error && g_asked.waiting)
					map_again("the engine answered the hello asking for a larger ring with an error");
			}
		}

		void basis_to_json(const view::Pose &p, float fwd[3], float up[3])
		{
			for (int i = 0; i < 3; ++i)
			{
				fwd[i] = -p.col[2][i];
				up[i] = p.col[1][i];
			}
		}
	}

	void start(int port, const std::string &token)
	{
		g_token = token;
		g_port = port;
		g_ws.start(g_host.c_str(), port);
		g_outbox.start([](const std::string &m) { return g_ws.send(m); }, [] { return g_ws.generation(); });
		logf("link: engine bridge at 127.0.0.1:%d", port);
	}

	void stop()
	{
		g_outbox.stop(); // after the send in flight (its timeout bounds it)
		if (g_stopper.joinable())
			g_stopper.join();
		g_ws.stop();
	}

	bool connected()
	{
		return g_ws.connected();
	}

	int &camera_delay()
	{
		return g_delay;
	}

	float &size()
	{
		return g_size;
	}

	bool &enabled()
	{
		return g_enabled;
	}

	void frame(int bw, int bh)
	{
		provider::State &ps = provider::state();
		// a provider that reads the game's camera at presentation does it now: its camera is this frame's
		const uint64_t cams_before = ps.cameras.count();
		const bool called = provider::run_present_callback();
		const bool presented_now = called && ps.cameras.count() > cams_before;
		g_stats.present_callback = called;
		receive();
		const int64_t now = provider::now_ms();
		provider::Camera newest, shown;
		const bool have_cam = ps.cameras.get(0, newest);
		provider::Avatar av;
		const bool have_av = ps.avatar.read(av) && now - av.at_ms < kStaleMs;
		const bool fresh = have_cam && now - newest.at_ms < kStaleMs;
		g_stats.provider_fresh = fresh && have_av;
		g_stats.provider_visible = ps.visible;
		g_stats.camera_age_ms = have_cam ? now - newest.at_ms : -1;

		if (g_parked)
		{
			layer::set_active(false);
			if (!g_backoff.due(now))
				return;
			if (g_stopper.joinable())
				g_stopper.join();
			g_parked = false;
			g_ws.start(g_host.c_str(), g_port); // a new connection: hello again
		}
		const bool live = g_ws.connected() && fresh && have_av && ps.visible && g_enabled;
		layer::set_active(live);
		if (!g_ws.connected())
		{
			g_answered = false;
			return;
		}
		// her picture: the game's size (1:1 with its pixels), within the largest ring the engine makes
		const hello_rules::Size picture = hello_rules::picture_for(bw, bh);
		if (picture.known())
		{
			g_viewW = picture.w;
			g_viewH = picture.h;
		}
		if (g_ws.generation() != g_generation)
		{
			// a new connection: say who we are and how large her picture is; her pose follows with the next frame
			g_generation = g_ws.generation();
			g_answered = false;
			g_capsLight = g_capsCues = false;
			g_sentIndex = ~uint64_t(0);
			if (!say_hello(g_asked.first(picture, now)))
				g_generation = -1; // not queued: again next frame
		}
		else if (g_answered && g_asked.lost(now))
			map_again("no reply in 2 s to the hello asking for a larger ring");
		else if (const hello_rules::Size more = g_asked.again(picture); more.known())
			hello_again(more, now); // the game's window grew past the ring
		if (!live || bw <= 0 || bh <= 0)
			return;

		// the floating origin follows her in big steps
		if (!g_originSet || std::fabs(av.pos[0] - g_origin[0]) > kRebase || std::fabs(av.pos[1] - g_origin[1]) > kRebase ||
			std::fabs(av.pos[2] - g_origin[2]) > kRebase)
		{
			for (int i = 0; i < 3; ++i)
				g_origin[i] = std::floor(av.pos[i] / 1000.0) * 1000.0;
			g_originSet = true;
			++g_stats.rebases;
			logf("link: origin %.0f %.0f %.0f", g_origin[0], g_origin[1], g_origin[2]);
		}

		// her placement and her animator parameters - raw facts; what they mean is her animation set's call
		const double feet[3] = {av.pos[0] - g_origin[0], av.pos[1] - g_origin[1], av.pos[2] - g_origin[2]};
		char up[96] = "", vel[96] = "";
		if (av.has_up)
			std::snprintf(up, sizeof(up), ",\"up\":[%.5f,%.5f,%.5f]", av.up[0], av.up[1], av.up[2]);
		if (av.has_vel)
			std::snprintf(vel, sizeof(vel), ",\"vel\":[%.3f,%.3f,%.3f]", av.vel[0], av.vel[1], av.vel[2]);
		const float scale = std::clamp(av.scale * g_size, 0.05f, 20.0f);
		// which point of her pos names (a seat: her hips), and the provider's own parameters beside speed/airborne
		char anchor[64] = "", extra[900] = "";
		if (const std::string point = ps.anchor(); point != "feet")
			std::snprintf(anchor, sizeof(anchor), ",\"anchor\":\"%s\"", point.c_str());
		size_t used = 0;
		for (const provider::Param &p : ps.take_params())
		{
			const int n = p.clear ? std::snprintf(extra + used, sizeof(extra) - used, ",\"%s\":null", p.name.c_str())
								  : std::snprintf(extra + used, sizeof(extra) - used, ",\"%s\":%.5g", p.name.c_str(), double(p.value));
			if (n <= 0 || used + size_t(n) >= sizeof(extra))
			{
				extra[used] = 0; // 16 names of 32 characters fit: never reached
				break;
			}
			used += size_t(n);
		}
		sendf("{\"t\":\"avatar\",\"id\":0,\"pos\":[%.4f,%.4f,%.4f],\"fwd\":[%.5f,%.5f,%.5f]%s%s%s,\"scale\":%.3f,\"params\":{\"speed\":%.3f,\"airborne\":%s%s}}",
			feet[0], feet[1], feet[2], av.fwd[0], av.fwd[1], av.fwd[2], up, vel, anchor, scale, av.speed, av.airborne ? "true" : "false", extra);

		// the provider's animation cues (only to an engine that takes them)
		for (const provider::Cue &c : ps.take_cues())
		{
			if (!g_capsCues)
				continue;
			if (c.kind == "play")
				sendf("{\"t\":\"play\",\"state\":\"%s\"}", c.name.c_str());
			else if (c.kind == "trigger")
				sendf("{\"t\":\"trigger\",\"name\":\"%s\"}", c.name.c_str());
			else
				sendf("{\"t\":\"resume\"}");
		}

		// the game's sun, a few times a second; and her shadow's strength on the game's ground
		provider::Light light;
		const bool have_light = ps.light.read(light) && now - light.at_ms < kLightStaleMs;
		if (have_light && g_capsLight && GetTickCount64() >= g_nextLight)
		{
			g_nextLight = GetTickCount64() + kLightEveryMs;
			const float m = std::max({light.color[0], light.color[1], light.color[2], 1e-6f});
			sendf("{\"t\":\"light\",\"sun\":{\"dir\":[%.4f,%.4f,%.4f],\"color\":[%.3f,%.3f,%.3f],\"intensity\":%.3f,\"visible\":%.2f}}", light.toward[0],
				light.toward[1], light.toward[2], light.color[0] / m, light.color[1] / m, light.color[2] / m, light.intensity * m, light.visible);
		}
		const float sun_up = have_light ? std::clamp(light.toward[1] * 5.0f, 0.0f, 1.0f) : 0.0f;
		layer::set_shadow(have_light ? sun_up * light.visible : 0.0f);

		// the game's NEWEST camera goes last: the engine draws the moment it reads it (once per game camera: a
		// present without a new one asks for no new picture)
		const float bb_aspect = float(bw) / float(bh);
		const float tan_new = view::tan_half_vertical(newest.fov_deg, newest.fov_horizontal != 0, newest.aspect > 0.0f ? newest.aspect : bb_aspect);
		view::Pose sent;
		if (newest.index != g_sentIndex && view::make_pose(newest.pos, newest.fwd, newest.up, tan_new, sent))
		{
			g_sentIndex = newest.index;
			const float fov_y = std::clamp(2.0f * std::atan(tan_new) * 57.29577951f, 1.0f, 179.0f);
			float f[3], u[3];
			basis_to_json(sent, f, u);
			const unsigned long long id = ++g_camId;
			sendf("{\"t\":\"cam\",\"id\":%llu,\"pos\":[%.4f,%.4f,%.4f],\"fwd\":[%.5f,%.5f,%.5f],\"up\":[%.5f,%.5f,%.5f],\"fovY\":%.4f,\"near\":%.3f,"
				  "\"far\":%.1f,\"w\":%d,\"h\":%d}",
				id, newest.pos[0] - g_origin[0], newest.pos[1] - g_origin[1], newest.pos[2] - g_origin[2], f[0], f[1], f[2], u[0], u[1], u[2], fov_y,
				std::max(newest.near_m, 0.05f), kFar, g_viewW, g_viewH);
			++g_stats.cams;
			std::lock_guard<std::mutex> lock(g_historyLock);
			History &h = g_history[id % kHistory];
			h.id = id;
			h.index = newest.index;
			h.pose = sent;
			h.aspect = float(g_viewW) / float(g_viewH);
			std::copy(av.pos, av.pos + 3, h.feet);
			std::copy(g_origin, g_origin + 3, h.origin);
		}

		// the camera being PRESENTED: the one the present callback just wrote, else `camera_delay` provider cameras
		// back (0 = the newest)
		if (!ps.cameras.get(presented_now ? 0 : uint64_t(std::max(g_delay, 0)), shown))
			shown = newest;
		const float tan_shown = view::tan_half_vertical(shown.fov_deg, shown.fov_horizontal != 0, shown.aspect > 0.0f ? shown.aspect : bb_aspect);
		view::Pose host;
		if (view::make_pose(shown.pos, shown.fwd, shown.up, tan_shown, host))
			layer::set_host(host, shown.near_m, shown.far_m);
		// its picture, if it was sent, is waited for (briefly) instead of re-projecting an older one
		unsigned long long want = 0;
		{
			std::lock_guard<std::mutex> lock(g_historyLock);
			for (const History &h : g_history)
				if (h.id != 0 && h.index == shown.index && h.id > want)
					want = h.id;
			// where she is in the presented frame: an older picture of her is moved by how far she went since
			const History &at = g_history[(want != 0 ? want : g_camId) % kHistory];
			g_shownFeetSet = at.id != 0;
			std::copy(at.feet, at.feet + 3, g_shownFeet);
		}
		layer::expect_frame(want);
	}

	bool pose_of(unsigned long long cam_id, view::Pose &out, float &aspect, double origin[3])
	{
		std::lock_guard<std::mutex> lock(g_historyLock);
		const History &h = g_history[cam_id % kHistory];
		if (h.id != cam_id || cam_id == 0)
			return false;
		out = h.pose;
		aspect = h.aspect;
		std::copy(h.origin, h.origin + 3, origin);
		return true;
	}

	bool moved_since(unsigned long long cam_id, double out[3])
	{
		std::lock_guard<std::mutex> lock(g_historyLock);
		const History &h = g_history[cam_id % kHistory];
		if (h.id != cam_id || !g_shownFeetSet)
			return false;
		for (int i = 0; i < 3; ++i)
			out[i] = g_shownFeet[i] - h.feet[i];
		return true;
	}

	Stats stats()
	{
		Stats s = g_stats;
		s.sent = g_outbox.sent();
		s.answered = g_answered && g_ws.connected();
		return s;
	}

	std::string last_event()
	{
		std::lock_guard<std::mutex> lock(g_eventLock);
		return g_event;
	}
}
