#include "config.h"
#include <windows.h>
#include <cwchar>
#include <cwctype>

namespace config
{
	namespace
	{
		std::wstring dir_of(const std::wstring &path)
		{
			const size_t slash = path.find_last_of(L"\\/");
			return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
		}

		/// A token is sent inside JSON: keep it to what a token is (no quotes, no control characters).
		std::string clean_token(const std::wstring &w)
		{
			std::string out;
			for (wchar_t c : w)
				if (c > 0x20 && c < 0x7F && c != L'"' && c != L'\\' && out.size() < 256)
					out.push_back(char(c));
			return out;
		}

		/// The value of -AstraConfig=<path> on the game's command line ("" if absent). Parsed by hand: no shell32 in
		/// the game's process. Quoting: the whole argument ("-AstraConfig=C:\a b\x.ini") or the value
		/// (-AstraConfig="C:\a b\x.ini"); unquoted, the value ends at the first space.
		std::wstring command_line_value(const wchar_t *line, const wchar_t *key)
		{
			const size_t n = std::wcslen(key);
			for (const wchar_t *p = line; *p != 0; ++p)
			{
				if (_wcsnicmp(p, key, n) != 0 || (p != line && p[-1] != L' ' && p[-1] != L'\t' && p[-1] != L'"'))
					continue;
				const bool quoted_arg = p != line && p[-1] == L'"';
				const wchar_t *v = p + n;
				const bool quoted_value = !quoted_arg && *v == L'"';
				if (quoted_value)
					++v;
				const wchar_t *end = v;
				while (*end != 0 && ((quoted_arg || quoted_value) ? *end != L'"' : (*end != L' ' && *end != L'\t')))
					++end;
				return std::wstring(v, end);
			}
			return std::wstring();
		}
	}

	Settings load()
	{
		Settings s;
		wchar_t exe[MAX_PATH] = L"";
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		s.log_path = dir_of(exe) + L"astra-reshade.log";
		s.source = L"defaults";

		// 1. -AstraConfig=<ini> on the command line
		const std::wstring ini = command_line_value(GetCommandLineW(), L"-AstraConfig=");
		if (!ini.empty() && GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			s.port = int(GetPrivateProfileIntW(L"bridge", L"Port", 25600, ini.c_str()));
			wchar_t token[300] = L"";
			GetPrivateProfileStringW(L"bridge", L"Token", L"", token, 300, ini.c_str());
			s.token = clean_token(token);
			s.log_path = dir_of(ini) + L"astra-reshade.log";
			s.source = ini;
			wchar_t v[64] = L"";
			if (GetPrivateProfileStringW(L"tone", L"Exposure", L"", v, 64, ini.c_str()) > 0)
			{
				const float e = std::wcstof(v, nullptr);
				if (e > 0.0f && e < 100.0f)
					s.tone_exposure = e;
			}
			GetPrivateProfileStringW(L"tone", L"Path", L"auto", v, 64, ini.c_str());
			s.tone_present = _wcsicmp(v, L"present") == 0;
			GetPrivateProfileStringW(L"tone", L"Engine", L"", v, 64, ini.c_str());
			s.tone_engine = clean_token(v);
		}
		else
		{
			// 2. the environment (a game Astra started itself, or a hand-made launch option)
			wchar_t v[300];
			if (GetEnvironmentVariableW(L"ASTRA_BRIDGE_PORT", v, 300) > 0)
			{
				s.port = int(std::wcstol(v, nullptr, 10));
				s.source = L"environment";
			}
			if (GetEnvironmentVariableW(L"ASTRA_BRIDGE_TOKEN", v, 300) > 0)
			{
				s.token = clean_token(v);
				s.source = L"environment";
			}
			if (GetEnvironmentVariableW(L"ASTRA_TONE_EXPOSURE", v, 300) > 0)
			{
				const float e = std::wcstof(v, nullptr);
				if (e > 0.0f && e < 100.0f)
					s.tone_exposure = e;
			}
			if (GetEnvironmentVariableW(L"ASTRA_TONE_PATH", v, 300) > 0)
				s.tone_present = _wcsicmp(v, L"present") == 0;
			if (GetEnvironmentVariableW(L"ASTRA_TONE_ENGINE", v, 300) > 0)
				s.tone_engine = clean_token(v);
			if (!ini.empty())
				s.source = L"defaults (-AstraConfig file not found: " + ini + L")";
		}
		if (s.port <= 0 || s.port > 65535)
			s.port = 25600;
		return s;
	}
}
