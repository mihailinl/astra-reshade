// astra-reshade: Astra, the Astra companion's avatar, composited into any D3D11/D3D12 game ReShade runs in
// (Linux: Proton/vkd3d with WINEDLLOVERRIDES="dxgi=n,b"). The game knows nothing about her: a PROVIDER mod in
// the same process (include/astra_provider.h; for Unreal, the AstraUE UE4SS mod over the pipe) says where the
// game's camera is and where she stands, and this add-on does the rest:
//   provider.cpp  the provider API: what the game's mod said, seqlocked for the present thread
//   pipe.cpp      the same API over \\.\pipe\astra-provider, for mods that cannot call native code
//   link.cpp      the Astra avatar engine's game bridge: her placement and the camera out, her pictures back
//   layer.cpp     her pictures into AstraPassthrough.fx: re-projected onto the presented camera, depth-tested
// Shared with Minecraft in Death Stranding (extern/minecraft-in-death-stranding, MIT): the WebSocket client (ws)
// and fault containment (guard).
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "config.h"
#include "guard.h"
#include "layer.h"
#include "link.h"
#include "log.h"
#include "pipe.h"
#include "provider.h"
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>

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

	void load_overlay_settings()
	{
		int delay = enginelink::camera_delay();
		reshade::get_config_value(nullptr, "ASTRA", "CameraDelay", delay);
		enginelink::camera_delay() = std::clamp(delay, 0, 3);
		float wait = layer::wait_ms();
		reshade::get_config_value(nullptr, "ASTRA", "WaitMs", wait);
		layer::wait_ms() = std::clamp(wait, 0.0f, 12.0f);
		float size = enginelink::size();
		reshade::get_config_value(nullptr, "ASTRA", "Size", size);
		enginelink::size() = std::clamp(size, 0.5f, 2.0f);
		bool on = enginelink::enabled();
		reshade::get_config_value(nullptr, "ASTRA", "Enabled", on);
		enginelink::enabled() = on;
	}

	void save_overlay_settings()
	{
		reshade::set_config_value(nullptr, "ASTRA", "CameraDelay", enginelink::camera_delay());
		reshade::set_config_value(nullptr, "ASTRA", "WaitMs", layer::wait_ms());
		reshade::set_config_value(nullptr, "ASTRA", "Size", enginelink::size());
		reshade::set_config_value(nullptr, "ASTRA", "Enabled", enginelink::enabled());
	}

	/// At the start of Present, before ReShade renders its effects: her placement and the newest camera go to
	/// the engine, and the compositor learns the camera of the frame being presented. (After the effects - the
	/// reshade_present event - her picture would land one frame behind the world: a ghost on every fast turn.)
	void on_present(command_queue *, swapchain *swap, const rect *, const rect *, uint32_t, const rect *)
	{
		++g_frame;
		if (!g_started)
		{
			g_started = true; // threads start here, not in DllMain (loader lock)
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
		enginelink::frame(int(g_bbWidth), int(g_bbHeight));
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
		bool changed = false;
		changed |= ImGui::Checkbox("Show Astra", &enginelink::enabled());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140);
		changed |= ImGui::SliderFloat("size", &enginelink::size(), 0.5f, 2.0f, "x%.2f");
		ImGui::SetNextItemWidth(140);
		changed |= ImGui::SliderInt("camera delay (frames)", &enginelink::camera_delay(), 0, 3);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How many frames the picture on screen is behind the game's newest camera.\n"
							  "Turn the camera fast: if she slides AHEAD of the turn, raise it; if she lags behind it, lower it.");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140);
		changed |= ImGui::SliderFloat("wait for her picture", &layer::wait_ms(), 0.0f, 12.0f, "%.1f ms");
		ImGui::SameLine();
		ImGui::TextDisabled("(late %llu times)", layer::wait_timeouts());
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
		g_settings = config::load();
		_wfopen_s(&g_log, g_settings.log_path.c_str(), L"a");
		logf("---- Astra (astra-reshade) loaded; settings from %ls; engine port %d%s", g_settings.source.c_str(), g_settings.port,
			g_settings.token.empty() ? "" : ", token set");
		guard::install();
		load_overlay_settings();
		layer::install();
		reshade::register_event<reshade::addon_event::present>(on_present);
		reshade::register_overlay("Astra", on_overlay);
		break;
	case DLL_PROCESS_DETACH:
		if (reserved == nullptr) // FreeLibrary; on process exit the threads are already gone, joining would hang
		{
			pipeserver::stop();
			enginelink::stop();
		}
		reshade::unregister_addon(module);
		if (g_log)
			std::fclose(g_log);
		break;
	}
	return TRUE;
}
