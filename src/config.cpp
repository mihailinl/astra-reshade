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
	}

	Settings load(const ReShadeKeys &reshade_ini)
	{
		Settings s;
		wchar_t exe[MAX_PATH] = L"";
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		s.log_path = dir_of(exe) + L"astra-reshade.log";

		const std::wstring ini = config_rules::option_value(GetCommandLineW(), L"-AstraConfig");
		const bool ini_ok = !ini.empty() && readable(ini);

		std::vector<config_rules::BridgeSource> sources(3);
		// 1. the -AstraConfig ini
		sources[0].name = ini;
		sources[0].present = ini_ok;
		if (ini_ok)
		{
			sources[0].port = ini_value(ini, L"bridge", L"Port");
			sources[0].token = ini_value(ini, L"bridge", L"Token");
		}
		// 2. the environment (a hand-made launch option)
		sources[1].name = L"environment";
		sources[1].port = env_value(L"ASTRA_BRIDGE_PORT", sources[1].present);
		sources[1].token = env_value(L"ASTRA_BRIDGE_TOKEN", sources[1].present);
		// 3. ReShade.ini [ASTRA]
		sources[2].name = L"ReShade.ini [ASTRA]";
		sources[2].present = !reshade_ini.port.empty() || !reshade_ini.token.empty();
		sources[2].port = reshade_ini.port;
		sources[2].token = reshade_ini.token;

		const config_rules::Bridge b = config_rules::pick_bridge(sources);
		s.port = b.port;
		s.token = b.token;
		s.source = b.source;
		if (!ini.empty() && !ini_ok)
			s.source += L" (-AstraConfig file missing or unreadable: " + ini + L")";

		// [tone]: the -AstraConfig ini's, else the environment's
		if (ini_ok)
		{
			s.log_path = dir_of(ini) + L"astra-reshade.log";
			read_exposure(ini_value(ini, L"tone", L"Exposure"), s.tone_exposure);
			s.tone_present = _wcsicmp(ini_value(ini, L"tone", L"Path").c_str(), L"present") == 0;
			s.tone_engine = config_rules::clean_token(ini_value(ini, L"tone", L"Engine"));
		}
		else
		{
			bool set = false;
			read_exposure(env_value(L"ASTRA_TONE_EXPOSURE", set), s.tone_exposure);
			s.tone_present = _wcsicmp(env_value(L"ASTRA_TONE_PATH", set).c_str(), L"present") == 0;
			s.tone_engine = config_rules::clean_token(env_value(L"ASTRA_TONE_ENGINE", set));
		}
		return s;
	}
}
