// astra-reshade: Astra, the Astra companion's avatar, composited into any D3D11/D3D12 game ReShade runs in
// (Linux: Proton/vkd3d with WINEDLLOVERRIDES="dxgi=n,b"). The game knows nothing about her: a PROVIDER mod in
// the same process (include/astra_provider.h; for Unreal, the AstraUE UE4SS mod over the pipe) says where the
// game's camera is and where she stands, and this add-on does the rest:
//   provider.cpp  the provider API: what the game's mod said, seqlocked for the present thread
//   pipe.cpp      the same API over \\.\pipe\astra-provider, for mods that cannot call native code
//   link.cpp      the Astra avatar engine's game bridge: her placement and the camera out, her pictures back
//   layer.cpp     her pictures into AstraPassthrough.fx: re-projected onto the presented camera, depth-tested
//   pretone.cpp   the effects drawn before the game's tonemapper (its grade and UI over her), else at Present
// Shared with Minecraft in Death Stranding (extern/minecraft-in-death-stranding, MIT): the WebSocket client (ws)
// and fault containment (guard).
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "camera_pace.h"
#include "config.h"
#include "config_rules.h"
#include "guard.h"
#include "layer.h"
#include "link.h"
#include "log.h"
#include "pipe.h"
#include "pretone.h"
#include "provider.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "Astra";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
	"Astra, the Astra companion's avatar, in any game: her engine draws her from the game's camera, this composites her with depth.";

namespace
{
	FILE *g_log = nullptr;
	uint64_t g_frame = 0;
	config::Settings g_settings;
	uint32_t g_bbWidth = 0, g_bbHeight = 0;
	bool g_started = false;
	device *g_presenter = nullptr; // the device whose swap chain started the threads
	std::mutex g_frameLock;
	bool g_frameBegun = false; // the engine link's frame ran for the frame being drawn (at its tonemapper)
	bool g_begunEarly = false; // ...and it ran there, before Present
	uint64_t g_camerasAtBegin = 0;
	// how often the game's newest camera changed between her pass (at the tonemapper) and Present: her picture's
	// camera is picked counting back from the newest, so a camera arriving in between would make her a frame late
	uint64_t g_earlyFrames = 0, g_camerasBetween = 0;
	ULONGLONG g_nextTimingLog = 0;

	/// A value of ReShade.ini through ReShade's API, its list elements each ended by '\0' ("" absent).
	std::string reshade_ini_value(const char *section, const char *key)
	{
		size_t size = 0;
		if (!reshade::get_config_value(nullptr, section, key, nullptr, &size) || size == 0)
			return std::string();
		std::string v(size, '\0');
		if (!reshade::get_config_value(nullptr, section, key, v.data(), &size))
			return std::string();
		v.resize(std::min(size, v.size()));
		return v;
	}

	/// An ASCII value of ReShade.ini as text (its list separators dropped).
	std::wstring widen(const std::string &s)
	{
		std::wstring out;
		for (const char c : s)
			if (c != 0)
				out.push_back(wchar_t(static_cast<unsigned char>(c)));
		return out;
	}

	/// ReShade.ini as her effect needs it, with no ReShade.ini shipped (config_rules.h): her shaders' folder added to
	/// EffectSearchPaths, and the keys in config_rules::kSeeds set where missing. Runs while ReShade loads add-ons, at
	/// the game's first device - before any swap chain, so before the effect runtime reads its config (its constructor,
	/// from the same in-memory ReShade.ini, which ReShade writes to disk later). Returns the keys it set, for the log.
	std::string seed_reshade_ini()
	{
		std::string set;
		const std::string raw = reshade_ini_value("GENERAL", "EffectSearchPaths");
		std::vector<std::string> paths = config_rules::split_list(raw.data(), raw.size());
		if (config_rules::add_search_path(paths, config_rules::kEffectSearchPath))
		{
			const std::string list = config_rules::join_list(paths);
			reshade::set_config_value(nullptr, "GENERAL", "EffectSearchPaths", list.data(), list.size());
			set += " EffectSearchPaths";
		}
		for (const config_rules::Seed &s : config_rules::kSeeds)
		{
			if (!reshade_ini_value(s.section, s.key).empty())
				continue;
			reshade::set_config_value(nullptr, s.section, s.key, s.value);
			set += ' ';
			set += s.key;
		}
		return set;
	}

	void load_overlay_settings()
	{
		int delay = enginelink::camera_delay();
		reshade::get_config_value(nullptr, "ASTRA", "CameraDelay", delay);
		enginelink::camera_delay() = std::clamp(delay, 0, camera_pace::kAgeCeilingMs);
		bool delayAuto = enginelink::camera_delay_auto();
		reshade::get_config_value(nullptr, "ASTRA", "CameraDelayAuto", delayAuto);
		enginelink::camera_delay_auto() = delayAuto;
		float wait = layer::wait_ms();
		reshade::get_config_value(nullptr, "ASTRA", "WaitMs", wait);
		layer::wait_ms() = std::clamp(wait, 0.0f, 12.0f);
		float size = enginelink::size();
		reshade::get_config_value(nullptr, "ASTRA", "Size", size);
		enginelink::size() = std::clamp(size, 0.5f, 2.0f);
		bool on = enginelink::enabled();
		reshade::get_config_value(nullptr, "ASTRA", "Enabled", on);
		enginelink::enabled() = on;
		// [tone] of the -AstraConfig ini (the launcher's, per game) first; ReShade.ini's overlay value over it
		float exposure = g_settings.tone_exposure;
		reshade::get_config_value(nullptr, "ASTRA", "ToneExposure", exposure);
		layer::tone_exposure() = std::clamp(exposure, 0.1f, 8.0f);
		pretone::path() = g_settings.tone_present ? 1 : 0;
	}

	void save_overlay_settings()
	{
		reshade::set_config_value(nullptr, "ASTRA", "CameraDelay", enginelink::camera_delay());
		reshade::set_config_value(nullptr, "ASTRA", "CameraDelayAuto", enginelink::camera_delay_auto());
		reshade::set_config_value(nullptr, "ASTRA", "WaitMs", layer::wait_ms());
		reshade::set_config_value(nullptr, "ASTRA", "Size", enginelink::size());
		reshade::set_config_value(nullptr, "ASTRA", "Enabled", enginelink::enabled());
		reshade::set_config_value(nullptr, "ASTRA", "ToneExposure", layer::tone_exposure());
	}

	/// The engine link's frame - her placement and the newest camera out, the presented camera to the compositor -
	/// once per frame: at the game's tonemapper when her pass is drawn there (pretone.cpp), else at Present.
	void begin_frame()
	{
		std::lock_guard<std::mutex> lock(g_frameLock);
		if (g_frameBegun)
			return;
		g_frameBegun = true;
		g_camerasAtBegin = provider::state().cameras.count();
		enginelink::frame(int(g_bbWidth), int(g_bbHeight));
	}

	/// At the start of Present, before ReShade renders its effects: her placement and the newest camera go to
	/// the engine, and the compositor learns the camera of the frame being presented. (After the effects - the
	/// reshade_present event - her picture would land one frame behind the world: a ghost on every fast turn.)
	void on_present(command_queue *, swapchain *swap, const rect *, const rect *, uint32_t, const rect *)
	{
		pretone::present_begins(); // first: no pass of hers on a recording thread from here to ReShade's Present work's end
		++g_frame;
		if (!g_started)
		{
			g_started = true; // threads start here, not in DllMain (loader lock)
			g_presenter = swap != nullptr ? swap->get_device() : nullptr;
			pipeserver::start();
			enginelink::start(g_settings.port, g_settings.token);
		}
		if (swap != nullptr)
		{
			const resource bb = swap->get_current_back_buffer();
			if (bb.handle != 0)
			{
				const resource_desc d = swap->get_device()->get_resource_desc(bb);
				g_bbWidth = d.texture.width;
				g_bbHeight = d.texture.height;
			}
		}
		{
			std::lock_guard<std::mutex> lock(g_frameLock);
			g_begunEarly = g_frameBegun;
		}
		begin_frame(); // nothing if it ran at this frame's tonemapper
		{
			std::lock_guard<std::mutex> lock(g_frameLock);
			if (g_begunEarly && enginelink::stats().provider_fresh)
			{
				++g_earlyFrames;
				g_camerasBetween += provider::state().cameras.count() != g_camerasAtBegin;
			}
			g_frameBegun = false; // the next frame's tonemapper begins the next one
		}
		if (GetTickCount64() >= g_nextTimingLog)
		{
			if (g_nextTimingLog != 0 && g_earlyFrames > 0)
				logf("timing: last 30 s, %llu of %llu frames got a newer game camera between her pass and Present", (unsigned long long)g_camerasBetween,
					(unsigned long long)g_earlyFrames);
			g_earlyFrames = g_camerasBetween = 0;
			g_nextTimingLog = GetTickCount64() + 30000;
		}
		pretone::presented(swap, g_bbWidth, g_bbHeight);
	}

	/// The threads stop with the device they started for - here, not in DllMain: ReShade unloads its add-ons right
	/// after the last device is released (a game's exit; Unreal also makes and drops probe devices first), and a
	/// thread joined under the loader lock never ends, it needs that lock to exit. A later device starts them again.
	void on_destroy_device(device *dev)
	{
		if (!g_started || dev != g_presenter)
			return;
		pipeserver::stop();
		enginelink::stop();
		g_started = false;
		g_presenter = nullptr;
		logf("the device the link started for is gone: its threads stopped");
	}

	void on_overlay(effect_runtime *)
	{
		const enginelink::Stats ls = enginelink::stats();
		const pipeserver::Stats ps = pipeserver::stats();
		ImGui::Text("Engine: %s   sent %llu  received %llu  cameras %llu  pictures %llu",
			ls.answered ? "linked" : (enginelink::connected() ? "connected, no hello yet" : "waiting (is Astra running?)"), ls.sent, ls.received, ls.cams,
			layer::frames());
		const std::string ev = enginelink::last_event();
		if (!ev.empty())
			ImGui::TextDisabled("last: %s", ev.c_str());
		ImGui::Text("Provider: %s%s   camera %lld ms ago   pipe: %s, %llu lines (%llu malformed)%s", ls.provider_fresh ? "live" : "silent",
			ls.provider_visible ? "" : " (hidden by the game)", ls.camera_age_ms, ps.connected ? "connected" : (ps.serving ? "waiting" : "not serving"), ps.lines,
			ps.malformed, provider::refused() ? "  REFUSED values" : "");
		ImGui::TextDisabled("%s", layer::status());
		ImGui::TextDisabled("%s", pretone::status());
		bool changed = false;
		changed |= ImGui::Checkbox("Show Astra", &enginelink::enabled());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140);
		changed |= ImGui::SliderFloat("size", &enginelink::size(), 0.5f, 2.0f, "x%.2f");
		if (ls.present_callback)
			ImGui::TextDisabled("camera: the presented frame's (the provider reads it at presentation)");
		else
		{
			changed |= ImGui::Checkbox("auto delay", &enginelink::camera_delay_auto());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Measures the wall-clock gap (ms) between the provider's cameras (a game whose engine ticks\n"
								  "slower than it presents, most Unreal games, needs more) instead of a fixed guess.");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(140);
			if (enginelink::camera_delay_auto())
			{
				int shown = enginelink::camera_delay_effective();
				ImGui::BeginDisabled();
				ImGui::SliderInt("camera delay (ms)", &shown, 0, camera_pace::kAgeCeilingMs);
				ImGui::EndDisabled();
			}
			else
			{
				changed |= ImGui::SliderInt("camera delay (ms)", &enginelink::camera_delay(), 0, camera_pace::kAgeCeilingMs);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("How many milliseconds old the picture on screen is behind the game's newest camera.\n"
									  "Turn the camera fast: if she slides AHEAD of the turn, raise it; if she lags behind it, lower it.");
			}
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140);
		changed |= ImGui::SliderFloat("wait for her picture", &layer::wait_ms(), 0.0f, 12.0f, "%.1f ms");
		ImGui::SameLine();
		ImGui::TextDisabled("(late %llu times)", layer::wait_timeouts());
		ImGui::SetNextItemWidth(140);
		changed |= ImGui::SliderFloat("her exposure in the game's light", &layer::tone_exposure(), 0.25f, 4.0f, "x%.2f", ImGuiSliderFlags_Logarithmic);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Drawn before the game's tonemapper, she goes through its tone curve: if she looks too dark or too bright\n"
							  "against the world in good light, move this ([tone] Exposure).");
		if (changed)
			save_overlay_settings();
		if (guard::contained() > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%d fault(s) contained (see astra-reshade.log)", guard::contained());
	}
}

void logf(const char *fmt, ...)
{
	if (g_log == nullptr)
		return;
	static std::mutex lock; // whole lines: the command-stream events come from the game's recording threads
	std::lock_guard<std::mutex> hold(lock);
	SYSTEMTIME t;
	GetLocalTime(&t);
	std::fprintf(g_log, "%02d:%02d:%02d.%03d f%llu ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, (unsigned long long)g_frame);
	va_list args;
	va_start(args, fmt);
	std::vfprintf(g_log, fmt, args);
	va_end(args);
	std::fputc('\n', g_log);
	std::fflush(g_log);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(module))
			return FALSE;
		{
			const std::string seeded = seed_reshade_ini();
			const config::ReShadeKeys astra { widen(reshade_ini_value("ASTRA", "Port")), widen(reshade_ini_value("ASTRA", "Token")) };
			g_settings = config::load(astra);
			_wfopen_s(&g_log, g_settings.log_path.c_str(), L"a");
			logf("---- Astra (astra-reshade) loaded; settings from %ls; engine port %d%s; tone: exposure %.2f, path %s%s%s", g_settings.source.c_str(),
				g_settings.port, g_settings.token.empty() ? "" : ", token set", g_settings.tone_exposure, g_settings.tone_present ? "present" : "auto",
				g_settings.tone_engine.empty() ? "" : ", engine ", g_settings.tone_engine.c_str());
			if (!seeded.empty())
				logf("ReShade.ini: set what her effect needs and it lacked:%s", seeded.c_str());
		}
		guard::install();
		load_overlay_settings();
		layer::install();
		pretone::install(begin_frame, g_settings.tone_engine);
		reshade::register_event<reshade::addon_event::present>(on_present);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		reshade::register_overlay("Astra", on_overlay);
		break;
	case DLL_PROCESS_DETACH:
		if (reserved == nullptr) // FreeLibrary; on process exit the threads are already gone, joining would hang
		{
			pipeserver::stop(); // already stopped with their device (on_destroy_device): nothing to join
			enginelink::stop();
		}
		guard::uninstall(); // never leave a vectored handler pointing into this module once it is unmapped
		reshade::unregister_addon(module);
		if (g_log)
			std::fclose(g_log);
		break;
	}
	return TRUE;
}
