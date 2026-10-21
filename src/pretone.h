#pragma once
// pretone: her composite BEFORE the game's tonemapper. ReShade draws its effects at Present, over the finished picture
// - over the game's UI and menus, and past its grade, bloom and vignette. This finds the game's tonemapper in its
// command stream (passfind.h) and runs the effects there instead, into a copy of the HDR scene colour the tonemapper
// is about to read, copied back - so the game's tone curve, grade, bloom, vignette and grain apply to her, and its UI is
// drawn over her. Her picture is mapped into the scene's light with tone.h. A game that renders below its output
// resolution (Unreal's screen percentage, TSR/DLSS/FSR) is drawn into at that lower render resolution, then upscaled
// with the game's own picture - once that size has held for a while (pretone_rules::Stability; a size still changing,
// dynamic resolution, is not drawn into).
//
// Only in a game whose provider (or the launcher's ini) declares it Unreal (pretone_rules.h): the tonemapper's shape
// and the tone curve inverted are Unreal's. Direct3D 11 and 12 (ReShade saves and restores neither a D3D12 command
// list's state nor a D3D11 context's - this module puts the game's state back after the effects itself, two
// different ways: D3D12 has no "read the state back" call, so it TRACKS what the game bound and replays it; D3D11's
// state is sticky and the context hands it back on request, so this just reads it with Get* right before the
// effects and writes it back with Set* right after). A D3D11 game's deferred contexts are not drawn into yet (her
// pass stays at Present for their frames). Anywhere else - no engine declared, no tonemapper found, HDR output,
// another API, [tone] Path=present - every event below returns at once and the effects run at Present as before
// pretone existed; a frame her pass at the tonemapper did not draw (the effects compiling for its target) is a frame
// without effects, then the Present path for a while. The log and the overlay say which path is used and why.
#include <cstdint>
#include <string>

namespace reshade::api
{
	struct swapchain;
}

namespace pretone
{
	/// From DllMain. `begin_frame` sends the engine this frame's camera (and reads the presented one): it runs at
	/// the tonemapper when her pass is drawn there, else at Present - once per frame either way. `ini_engine`: [tone]
	/// Engine= of the launcher's ini ("" none).
	void install(void (*begin_frame)(), const std::string &ini_engine);
	/// FIRST in the add-on's present event: from here until ReShade's Present work is done (reshade_present), her pass
	/// is not drawn on a recording thread - and one in flight is waited for.
	void present_begins();
	/// At the start of every Present: the frame ends; the presented picture's size and colour space are read, where
	/// she is drawn is decided again.
	void presented(reshade::api::swapchain *swap, uint32_t width, uint32_t height);
	/// 0: auto - before the tonemapper where the game's engine allows (default), 1: always at Present ([tone] Path).
	int &path();
	/// One line for the overlay: where she is drawn and why.
	const char *status();
}
