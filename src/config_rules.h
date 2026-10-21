#pragma once
// The add-on's settings rules, portable (no ReShade, no Windows): tests/core_tests.cpp runs them.
//   option_value()   -AstraConfig <ini> (or -AstraConfig=<ini>) on the game's command line: how Astra's launcher points
//                    the add-on at the ini it writes into its own profile for this game ([bridge] Port= / Token=)
//   steam_app_id()   SteamAppId / SteamGameId, as Steam gives them to the game's own process (inherited by every
//                    child, even one started by a launcher that drops OUR argv) - picks the appid out of either
//   steam_profile_ini()   the SAME ini, found without ANY argv: <astra games root>/<appid>/astra-reshade.ini
//   pick_bridge()    where the engine's port and token come from: the first source that is there decides both
//   clean_token()    a token as the engine's JSON can carry it
//   add_search_path() / kSeeds   the keys of ReShade's own ReShade.ini her effect needs and ReShade's defaults lack,
//                    set at load when missing. Astra's launcher ships no ReShade.ini: ReShade rewrites that file while
//                    the game runs, and every file the launcher places is checked against its digest at each Play.
#include <cstddef>
#include <cwchar>
#include <string>
#include <vector>

namespace config_rules
{
	constexpr int kDefaultPort = 25600;

	inline wchar_t lower(wchar_t c)
	{
		return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c;
	}

	inline bool blank(wchar_t c)
	{
		return c == L' ' || c == L'\t';
	}

	/// `line[at...]` starts with `word`, ignoring letter case (ASCII).
	inline bool starts_with_at(const std::wstring &line, size_t at, const wchar_t *word)
	{
		for (size_t i = 0; word[i] != 0; ++i)
			if (at + i >= line.size() || lower(line[at + i]) != lower(word[i]))
				return false;
		return true;
	}

	/// The value of the option `key` on a Windows command line, "" when it is absent or has none; the first one wins.
	/// Forms: `key <value>` (Astra's launcher passes the two arguments `-AstraConfig` `<path>`) and `key=<value>`; the key
	/// is matched whole (not `-AstraConfigX`) and ignoring letter case. Quoting: the value ("C:\a b\x.ini") or the whole
	/// argument ("-AstraConfig=C:\a b\x.ini"). Unquoted, the value ends at the first blank - except an .ini path with
	/// blanks in it (a launch that drops the quotes: `-AstraConfig C:\Users\A B\astra.ini -next`), which ends after the
	/// first ".ini" that ends a word before the next option. Parsed by hand: no shell32 in the game's process.
	inline std::wstring option_value(const std::wstring &line, const wchar_t *key)
	{
		const size_t n = std::wcslen(key);
		for (size_t p = 0; p + n <= line.size(); ++p)
		{
			if ((p > 0 && !blank(line[p - 1]) && line[p - 1] != L'"') || !starts_with_at(line, p, key))
				continue;
			const bool quoted_arg = p > 0 && line[p - 1] == L'"';
			size_t v = p + n;
			bool in_quotes = false;
			if (v == line.size())
				return std::wstring();
			if (line[v] == L'=')
			{
				++v;
				in_quotes = quoted_arg; // "-AstraConfig=C:\a b\x.ini"
			}
			else if (blank(line[v]) || (quoted_arg && line[v] == L'"'))
			{
				if (line[v] == L'"')
					++v; // the end of a quoted "-AstraConfig"
				while (v < line.size() && blank(line[v]))
					++v;
				if (v < line.size() && line[v] == L'-')
					return std::wstring(); // `-AstraConfig -next`: no value
			}
			else
				continue; // another option that starts the same: -AstraConfigX
			if (!in_quotes && v < line.size() && line[v] == L'"')
			{
				in_quotes = true;
				++v;
			}
			size_t end = v;
			if (in_quotes)
			{
				while (end < line.size() && line[end] != L'"')
					++end;
				return line.substr(v, end - v);
			}
			while (end < line.size() && !blank(line[end]))
				++end;
			if (!(end - v >= 4 && starts_with_at(line, end - 4, L".ini")))
			{
				// not an .ini word: an .ini path with blanks, if one ends before the next option
				for (size_t i = v; i + 4 <= line.size(); ++i)
				{
					if (blank(line[i]) && i + 1 < line.size() && line[i + 1] == L'-')
						break;
					if (starts_with_at(line, i, L".ini") && (i + 4 == line.size() || blank(line[i + 4]) || line[i + 4] == L'"'))
					{
						end = i + 4;
						break;
					}
				}
			}
			return line.substr(v, end - v);
		}
		return std::wstring();
	}

	/// Steam gives the game's own process its appid as `SteamAppId` (plain digits) or `SteamGameId` (digits,
	/// optionally `_<modid>`-suffixed for workshop content) - inherited by every child of that process, even one a
	/// launcher starts without forwarding OUR OWN argv (Poppy Playtime's PlaytimeLauncher does exactly that, which
	/// is why this exists: UE4SS's proxy then finds no `-AstraConfig`, but SteamAppId still reaches it). The first
	/// one present and parseable wins; "" when neither is.
	inline std::wstring steam_app_id(const std::wstring &steam_app_id_env, const std::wstring &steam_game_id_env)
	{
		const auto digits_prefix = [](const std::wstring &s) -> std::wstring
		{
			size_t n = 0;
			while (n < s.size() && s[n] >= L'0' && s[n] <= L'9')
				++n;
			return n > 0 && (n == s.size() || s[n] == L'_') ? s.substr(0, n) : std::wstring();
		};
		const std::wstring a = digits_prefix(steam_app_id_env);
		return !a.empty() ? a : digits_prefix(steam_game_id_env);
	}

	inline std::wstring no_trailing_slash(std::wstring s, wchar_t slash)
	{
		while (!s.empty() && s.back() == slash)
			s.pop_back();
		return s;
	}

	/// Astra's launcher profile ini for this game, found with NO argv at all:
	/// `<astra games root>/<app_id>/astra-reshade.ini`. `app_id` is `steam_app_id()`'s result; "" (no SteamAppId)
	/// means no path. `is_wine` says this process is Proton/Wine (config.cpp: ntdll's `wine_get_version` export, or
	/// `STEAM_COMPAT_DATA_PATH` set) - then the root is the LINUX one Astra's own daemon writes to: `home` and
	/// `xdg_data_home` are NOT `$HOME`/`$XDG_DATA_HOME` (Wine clears those from the Windows env this process gets -
	/// confirmed live, `wine cmd /c set` shows neither even when both are exported right before exec'ing wine) but
	/// `WINE_HOST_HOME` / `WINE_HOST_XDG_DATA_HOME`, which ntdll re-exposes instead (present the same way its Unix
	/// twin would be: `WINE_HOST_XDG_DATA_HOME` only when `$XDG_DATA_HOME` was set upstream). The root is
	/// `xdg_data_home/astra/games`, or else `home/.local/share/astra/games`, opened through `Z:`. Native Windows:
	/// `%APPDATA%\astra\astra\data\games` (ROAMING AppData - `directories`' `ProjectDirs::from("com","astra",
	/// "astra")`; NEVER %LOCALAPPDATA%). "" when `app_id` is empty, or the one env piece the matching branch needs
	/// (home/xdg under Wine, appdata natively) is missing too.
	inline std::wstring steam_profile_ini(bool is_wine, const std::wstring &app_id, const std::wstring &home,
		const std::wstring &xdg_data_home, const std::wstring &appdata)
	{
		if (app_id.empty())
			return std::wstring();
		if (is_wine)
		{
			const std::wstring base = !xdg_data_home.empty() ? xdg_data_home
				: (!home.empty() ? no_trailing_slash(home, L'/') + L"/.local/share" : std::wstring());
			if (base.empty())
				return std::wstring();
			return L"Z:" + no_trailing_slash(base, L'/') + L"/astra/games/" + app_id + L"/astra-reshade.ini";
		}
		if (appdata.empty())
			return std::wstring();
		return no_trailing_slash(appdata, L'\\') + L"\\astra\\astra\\data\\games\\" + app_id + L"\\astra-reshade.ini";
	}

	/// A token is sent inside JSON: keep it to what a token is (printable ASCII, no quotes or backslashes, <= 256).
	inline std::string clean_token(const std::wstring &w)
	{
		std::string out;
		for (const wchar_t c : w)
			if (c > 0x20 && c < 0x7F && c != L'"' && c != L'\\' && out.size() < 256)
				out.push_back(char(c));
		return out;
	}

	/// One place the engine's port and token may come from, as read.
	struct BridgeSource
	{
		std::wstring name;    // for the log: the ini's path, "environment", "ReShade.ini [ASTRA]"
		bool present = false; // the file is there and readable / a variable is set / a key is set
		std::wstring port;    // as written; "" none
		std::wstring token;
	};

	struct Bridge
	{
		int port = kDefaultPort;
		std::string token;
		std::wstring source = L"defaults";
	};

	/// A port as written ("25601", blanks around it allowed); 0 when it is not one (1..65535).
	inline int parse_port(const std::wstring &s)
	{
		size_t i = 0;
		while (i < s.size() && blank(s[i]))
			++i;
		long v = 0;
		size_t digits = 0;
		for (; i < s.size() && s[i] >= L'0' && s[i] <= L'9' && digits < 6; ++i, ++digits)
			v = v * 10 + (s[i] - L'0');
		while (i < s.size() && blank(s[i]))
			++i;
		return digits > 0 && i == s.size() && v >= 1 && v <= 65535 ? int(v) : 0;
	}

	/// The first source that is present decides both values: a source is one unit (Astra's launcher writes the port and
	/// this session's token together; an empty token there must not let a stale one from further down through). A port
	/// that is not 1..65535 is the default one. Nothing present: port 25600, no token.
	inline Bridge pick_bridge(const std::vector<BridgeSource> &sources)
	{
		Bridge b;
		for (const BridgeSource &s : sources)
		{
			if (!s.present)
				continue;
			const int port = parse_port(s.port);
			b.port = port != 0 ? port : kDefaultPort;
			b.token = clean_token(s.token);
			b.source = s.name;
			return b;
		}
		return b;
	}

	/// ReShade's list values come through its API as elements each ended by '\0'.
	inline std::vector<std::string> split_list(const char *data, size_t size)
	{
		std::vector<std::string> out;
		std::string cur;
		for (size_t i = 0; i < size; ++i)
		{
			if (data[i] != 0)
				cur.push_back(data[i]);
			else if (!cur.empty())
				out.push_back(std::move(cur)), cur.clear();
		}
		if (!cur.empty())
			out.push_back(std::move(cur));
		return out;
	}

	inline std::string join_list(const std::vector<std::string> &items)
	{
		std::string out;
		for (const std::string &s : items)
			out += s, out.push_back('\0');
		return out;
	}

	/// Where ReShade finds her effect (AstraPassthrough.fx, AstraGame.fxh, ReShade.fxh), relative to ReShade's folder:
	/// the game's exe folder, where astra.addon64 itself sits so that ReShade's DEFAULT AddonPath finds it.
	constexpr const char *kEffectSearchPath = ".\\astra-reshade\\Shaders\\**";

	/// A search path as Windows compares it: letter case and the kind of slash ignored.
	inline std::string path_key(const std::string &p)
	{
		std::string out;
		for (const char c : p)
			out.push_back(c == '/' ? '\\' : (c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c));
		return out;
	}

	/// Adds `ours` at the end of ReShade's EffectSearchPaths when it is not among them; false when nothing changed.
	/// A missing key reads as no paths, so the list becomes `ours` alone (ReShade's own default, ".\", would search the
	/// game's exe folder).
	inline bool add_search_path(std::vector<std::string> &paths, const std::string &ours)
	{
		const std::string key = path_key(ours);
		for (const std::string &p : paths)
			if (path_key(p) == key)
				return false;
		paths.push_back(ours);
		return true;
	}

	/// ReShade.ini keys set only when missing (a person's own value is never overwritten).
	struct Seed
	{
		const char *section, *key, *value;
	};
	constexpr Seed kSeeds[] = {
		// her effect's settings in her folder, not ReShadePreset.ini beside the game's exe
		{ "GENERAL", "PresetPath", ".\\astra-reshade\\AstraPreset.ini" },
		// ReShade's first-start tutorial banner skipped, as the ReShade.ini the manual install shipped did
		{ "OVERLAY", "TutorialProgress", "4" },
		// The game's depth buffer, which hides her behind walls (ReShade's own generic-depth add-on picks it, reading
		// these at the game's first device - after add-ons load, so these seeds reach its first session): Unreal draws
		// its scene below the output resolution (screen percentage, TSR, DLSS, FSR), and the default "similar aspect
		// ratio" rule throws out a depth buffer more than 1.85x smaller than the screen - Poppy Playtime draws at 768x436
		// for 2560x1440 and she showed through every wall (the owner, 2026-10-20: picking it by hand fixed it). So no
		// size rule (0), and only Unreal's scene depth format, D32S8 (6): its shadow maps are D16 and never chosen.
		{ "DEPTH", "UseAspectRatioHeuristics", "0" },
		{ "DEPTH", "FilterFormat", "6" },
	};
}
