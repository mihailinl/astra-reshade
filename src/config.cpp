#include "config.h"
#include "config_rules.h"
#include <windows.h>
#include <cwchar>

namespace config
{
	namespace
	{
		std::wstring dir_of(const std::wstring &path)
		{
			const size_t slash = path.find_last_of(L"\\/");
			return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
		}

		/// The file is there and this process may read it (a folder is not).
		bool readable(const std::wstring &path)
		{
			const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				return false;
			CloseHandle(h);
			return true;
		}

		std::wstring ini_value(const std::wstring &ini, const wchar_t *section, const wchar_t *key)
		{
			wchar_t v[300] = L"";
			GetPrivateProfileStringW(section, key, L"", v, 300, ini.c_str());
			return v;
		}

		/// A variable's value ("" unset or too long); `set` becomes true when it is set.
		std::wstring env_value(const wchar_t *name, bool &set)
		{
			wchar_t v[300] = L"";
			const DWORD n = GetEnvironmentVariableW(name, v, 300);
			if (n == 0 || n >= 300)
				return std::wstring();
			set = true;
			return v;
		}

		void read_exposure(const std::wstring &v, float &exposure)
		{
			if (v.empty())
				return;
			const float e = std::wcstof(v.c_str(), nullptr);
			if (e > 0.0f && e < 100.0f)
				exposure = e;
		}

		/// This process is Proton/Wine, not native Windows: ntdll exports wine_get_version (every Wine build does;
		/// native Windows never does), or Steam told this process where its Proton compat prefix is
		/// (STEAM_COMPAT_DATA_PATH - set for a Linux Steam launch before Windows env vars like %APPDATA% can even
		/// be asked). Either is enough on its own.
		bool running_under_wine()
		{
			bool set = false;
			if (!env_value(L"STEAM_COMPAT_DATA_PATH", set).empty())
				return true;
			const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
		}
	}

	Settings load(const ReShadeKeys &reshade_ini)
	{
		Settings s;
		wchar_t exe[MAX_PATH] = L"";
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		s.log_path = dir_of(exe) + L"astra-reshade.log";

		const std::wstring ini = config_rules::option_value(GetCommandLineW(), L"-AstraConfig");
		const bool ini_ok = !ini.empty() && readable(ini);

		// The same ini, found with no argv at all: <astra games root>/<SteamAppId>/astra-reshade.ini.
		bool set = false;
		const std::wstring steam_app_id_env = env_value(L"SteamAppId", set);
		set = false;
		const std::wstring steam_game_id_env = env_value(L"SteamGameId", set);
		const std::wstring app_id = config_rules::steam_app_id(steam_app_id_env, steam_game_id_env);
		set = false;
		const std::wstring home = env_value(L"HOME", set);
		set = false;
		const std::wstring xdg_data_home = env_value(L"XDG_DATA_HOME", set);
		set = false;
		const std::wstring appdata = env_value(L"APPDATA", set);
		const std::wstring profile_ini = config_rules::steam_profile_ini(running_under_wine(), app_id, home, xdg_data_home, appdata);
		const bool profile_ini_ok = !profile_ini.empty() && readable(profile_ini);

		std::vector<config_rules::BridgeSource> sources(4);
		// 1. the -AstraConfig ini
		sources[0].name = ini;
		sources[0].present = ini_ok;
		if (ini_ok)
		{
			sources[0].port = ini_value(ini, L"bridge", L"Port");
			sources[0].token = ini_value(ini, L"bridge", L"Token");
		}
		// 2. the profile ini found by SteamAppId, no argv needed
		sources[1].name = profile_ini.empty() ? L"profile (no SteamAppId)" : profile_ini;
		sources[1].present = profile_ini_ok;
		if (profile_ini_ok)
		{
			sources[1].port = ini_value(profile_ini, L"bridge", L"Port");
			sources[1].token = ini_value(profile_ini, L"bridge", L"Token");
		}
		// 3. the environment (a hand-made launch option)
		sources[2].name = L"environment";
		sources[2].port = env_value(L"ASTRA_BRIDGE_PORT", sources[2].present);
		sources[2].token = env_value(L"ASTRA_BRIDGE_TOKEN", sources[2].present);
		// 4. ReShade.ini [ASTRA]
		sources[3].name = L"ReShade.ini [ASTRA]";
		sources[3].present = !reshade_ini.port.empty() || !reshade_ini.token.empty();
		sources[3].port = reshade_ini.port;
		sources[3].token = reshade_ini.token;

		const config_rules::Bridge b = config_rules::pick_bridge(sources);
		s.port = b.port;
		s.token = b.token;
		s.source = b.source;
		if (!ini.empty() && !ini_ok)
			s.source += L" (-AstraConfig file missing or unreadable: " + ini + L")";

		// [tone]: whichever ini supplied the bridge settings above (-AstraConfig, else the SteamAppId-found
		// profile ini), else the environment.
		const bool have_ini = ini_ok || profile_ini_ok;
		const std::wstring &effective_ini = ini_ok ? ini : profile_ini;
		if (have_ini)
		{
			s.log_path = dir_of(effective_ini) + L"astra-reshade.log";
			read_exposure(ini_value(effective_ini, L"tone", L"Exposure"), s.tone_exposure);
			s.tone_present = _wcsicmp(ini_value(effective_ini, L"tone", L"Path").c_str(), L"present") == 0;
			s.tone_engine = config_rules::clean_token(ini_value(effective_ini, L"tone", L"Engine"));
		}
		else
		{
			bool set2 = false;
			read_exposure(env_value(L"ASTRA_TONE_EXPOSURE", set2), s.tone_exposure);
			s.tone_present = _wcsicmp(env_value(L"ASTRA_TONE_PATH", set2).c_str(), L"present") == 0;
			s.tone_engine = config_rules::clean_token(env_value(L"ASTRA_TONE_ENGINE", set2));
		}
		return s;
	}
}
