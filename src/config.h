#pragma once
// Where the add-on gets its settings, read once at load (the rules: config_rules.h). The engine's port and this
// session's token come from the first of these that is there:
//   1. the ini named on the game's command line: -AstraConfig <path> (or -AstraConfig=<path>), its [bridge] Port= /
//      Token=. Astra's launcher writes that ini into its own profile for this game at every Play and passes its path
//      (a Steam launch carries arguments, not environment); by hand it is the developer's override;
//   2. the environment: ASTRA_BRIDGE_PORT, ASTRA_BRIDGE_TOKEN (a hand-made launch option);
//   3. ReShade.ini's [ASTRA] Port= / Token=;
//   4. port 25600, no token.
// A named ini that is missing or unreadable is skipped, and the one load line in the log says so. The token is never
// logged, only whether there is one. The log goes beside that ini (or beside the game's exe): astra-reshade.log.
// The same ini's [tone] (game data the launcher may write): Engine=unreal declares the game's engine when its provider
// does not (pretone_rules.h: her pass goes before the game's tonemapper only in an Unreal game); Exposure= her
// brightness in the game's scene before its tone curve (1); Path=auto (before the tonemapper where the engine allows,
// the default) | present (always at Present). Without the ini, ASTRA_TONE_ENGINE / ASTRA_TONE_EXPOSURE / ASTRA_TONE_PATH.
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
