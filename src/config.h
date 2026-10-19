#pragma once
// Where the add-on gets its settings, read once at load (the rules: config_rules.h). The engine's port and this
// session's token come from the first of these that is there:
//   1. the ini named on the game's command line: -AstraConfig <path> (or -AstraConfig=<path>), its [bridge] Port= /
//      Token=. Astra's launcher writes that ini into its own profile for this game at every Play and passes its path
//      (a Steam launch carries arguments, not environment); by hand it is the developer's override;
//   2. the SAME ini, found with NO argv at all: <astra games root>/<SteamAppId>/astra-reshade.ini
//      (config_rules::steam_app_id / steam_profile_ini). SteamAppId (or SteamGameId) is in the env Steam gives the
//      game's own process, inherited even by a launcher that starts the real exe without forwarding OUR argv (the
//      failure this rung exists for: Poppy Playtime's PlaytimeLauncher does exactly that). The games root: native
//      Windows %APPDATA%\astra\astra\data\games (ROAMING, never %LOCALAPPDATA%); under Proton/Wine (detected in
//      config.cpp: ntdll's wine_get_version export, or STEAM_COMPAT_DATA_PATH set) the LINUX root Astra's daemon
//      writes to, from WINE_HOST_XDG_DATA_HOME/astra/games or WINE_HOST_HOME/.local/share/astra/games - NOT
//      XDG_DATA_HOME/HOME themselves: Wine clears those from the Windows env it gives this process (confirmed
//      live under proton-cachyos-slr) and re-exposes them under the WINE_HOST_ names instead - opened through Z:;
//   3. the environment: ASTRA_BRIDGE_PORT, ASTRA_BRIDGE_TOKEN (a hand-made launch option);
//   4. ReShade.ini's [ASTRA] Port= / Token=;
//   5. port 25600, no token.
// A named ini (1 or 2) that is missing or unreadable is skipped, and the one load line in the log says so. The
// token is never logged, only which rung it came from. The log goes beside whichever ini supplied the settings (1
// or 2), else beside the game's exe: astra-reshade.log.
// [tone] (game data the launcher may write) comes from whichever ini won the bridge settings above (1 or 2);
// Engine=unreal declares the game's engine when its provider does not (pretone_rules.h: her pass goes before the
// game's tonemapper only in an Unreal game); Exposure= her brightness in the game's scene before its tone curve
// (1); Path=auto (before the tonemapper where the engine allows, the default) | present (always at Present).
// Without either ini, ASTRA_TONE_ENGINE / ASTRA_TONE_EXPOSURE / ASTRA_TONE_PATH.
#include <string>

namespace config
{
	struct Settings
	{
		int port = 25600;
		std::string token;
		std::wstring log_path;
		std::wstring source; // what the port and token came from, for the log
		float tone_exposure = 1.0f;
		bool tone_present = false;
		std::string tone_engine; // [tone] Engine= ("" none)
	};

	/// ReShade.ini's [ASTRA] Port= / Token=, as main.cpp reads them through ReShade's API ("" absent).
	struct ReShadeKeys
	{
		std::wstring port, token;
	};

	Settings load(const ReShadeKeys &reshade_ini);
}
