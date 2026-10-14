#pragma once
// pretone: her composite BEFORE the game's tonemapper. ReShade draws its effects at Present, over the finished picture
// - over the game's UI and menus, and past its grade, bloom and vignette. This finds the game's tonemapper in its
// command stream (passfind.h) and runs the effects there instead, into a copy of the HDR scene colour the tonemapper
// is about to read, copied back - so the game's tone curve, grade, bloom, vignette and grain apply to her, and its UI is
// drawn over her. Her picture is mapped into the scene's light with tone.h.
//
// Direct3D 12 only (ReShade cannot save and restore a D3D12 command list's state, so this module tracks what the game
// bound and puts it back after the effects). Anywhere it cannot - no tonemapper found, HDR output, another API,
// [tone] Path=present - the effects run at Present as before, and the log and the overlay say which path is used.
#include <cstdint>

namespace reshade::api
{
	struct swapchain;
}

namespace pretone
{
	/// From DllMain. `begin_frame` sends the engine this frame's camera (and reads the presented one): it runs at
	/// the tonemapper when her pass is drawn there, else at Present - once per frame either way.
	void install(void (*begin_frame)());
	/// At the start of every Present: the frame ends; the presented picture's size and colour space are read.
	void presented(reshade::api::swapchain *swap, uint32_t width, uint32_t height);
	/// 0: before the tonemapper when it is found (default), 1: always at Present ([tone] Path = auto | present).
	int &path();
	/// One line for the overlay: where she is drawn and why.
	const char *status();
}
