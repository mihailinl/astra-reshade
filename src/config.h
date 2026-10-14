#pragma once
// Where the add-on gets its settings, read once at load:
//   1. the game's command line: -AstraConfig=<path to an .ini> (the launcher puts it there, pointing into
//      Astra's own profile folder for this game; a Steam launch carries arguments, not environment);
//      that ini's [bridge] Port= / Token= are the engine's port and this session's token;
//   2. else the environment: ASTRA_BRIDGE_PORT, ASTRA_BRIDGE_TOKEN;
//   3. else port 25600, no token.
// The log goes beside that ini (or beside the game's exe): astra-reshade.log.
// The same ini's [tone] (game data the launcher may write): Exposure= her brightness in the game's scene before its
// tone curve (1), Path=auto (her pass before the game's tonemapper when it is found) | present (always at Present).
// Without the ini, ASTRA_TONE_EXPOSURE / ASTRA_TONE_PATH.
#include <string>

namespace config
{
	struct Settings
	{
		int port = 25600;
		std::string token;
		std::wstring log_path;
		std::wstring source; // what the settings came from, for the log
		float tone_exposure = 1.0f;
		bool tone_present = false;
	};

	Settings load();
}
