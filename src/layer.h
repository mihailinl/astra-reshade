#pragma once
// Her picture in the game's: the engine's frames (shared memory, "MCPT": premultiplied colour + linear depth,
// crop-aware) are uploaded into AstraPassthrough.fx's textures; the effect re-projects the picture from the
// camera it was drawn with onto the camera being presented, and depth-tests it against the game's depth.
#include "view.h"

namespace layer
{
	/// From DllMain: subscribes to ReShade's effect events.
	void install();
	/// Composite her this frame (the engine is linked, a provider is live and she is shown).
	void set_active(bool active);
	bool active();
	/// The effects about to run draw into the game's HDR scene colour, before its tonemapper (pretone.cpp): her
	/// picture is mapped into the scene's light (tone.h), the game's own grade applies to her after.
	void set_hdr_pass(bool hdr);
	/// [tone] Exposure: her brightness in the game's scene, before its tone curve (1: as her engine drew her).
	float &tone_exposure();
	/// How many times the effects began (ReShade's begin-effects event): tells a caller of render_effects whether
	/// they ran (not while they compile for a new target, nor twice in a frame).
	unsigned long long effect_runs();
	/// Where the engine says its frames are ("/dev/shm/astra-frame" or "Local\\AstraFrame"): mapped again at
	/// every hello (a restarted engine's ring is a new file). Present thread only.
	void set_frames_at(const char *where);
	/// The camera being presented (absolute world) and the game's near and far clip (its depth buffer, reversed
	/// Z, is linearised with them; far 0: an infinite projection).
	void set_host(const view::Pose &host, float near_m, float far_m);
	/// How dark her shadow on the game's ground may be (0: none) - cast from her view from the sun, which the
	/// engine sends while a sun lights her.
	void set_shadow(float strength);
	/// The cam.id whose picture is waited for (up to wait_ms) before compositing; 0: none.
	void expect_frame(unsigned long long cam_id);
	/// How long to wait for that picture (ms; 0: never, re-project the newest instead). The overlay's slider.
	float &wait_ms();
	unsigned long long wait_timeouts();
	/// Pictures uploaded so far.
	unsigned long long frames();
	/// One line for the overlay.
	const char *status();
}
