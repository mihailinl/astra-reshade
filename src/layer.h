#pragma once
// Her picture in the game's: the engine's frames (shared memory, "MCPT": premultiplied colour + linear depth,
// crop-aware) are uploaded into AstraPassthrough.fx's textures; the effect re-projects the picture from the
// camera it was drawn with onto the camera being presented, and depth-tests it against the game's depth.
#include "view.h"
#include <cstdint>

namespace layer
{
	/// From DllMain: subscribes to ReShade's effect events.
	void install();
	/// Composite her this frame (the engine is linked, a provider is live and she is shown).
	void set_active(bool active);
	bool active();
	/// The effects about to run draw into the game's HDR scene colour, before its tonemapper (pretone.cpp), on a game
	/// thread recording command list `frame` presents in: her picture is mapped into the scene's light (tone.h), the
	/// game's own grade applies to her after, and her picture is uploaded through that command list.
	/// `frame`: the present it belongs to (pretone's count; set at every present too) - the clock of the staging ring
	/// her planes are uploaded from. `vp_w`/`vp_h` (hdr only): the tonemapper's viewport - her copy's size - so the
	/// depth uv scale (frame_meta's, for a game whose depth buffer is on a padded grid too) can be measured fresh
	/// every run, against whatever size ReShade's own depth add-on actually picked.
	void set_hdr_pass(bool hdr, unsigned long long frame, uint32_t vp_w = 0, uint32_t vp_h = 0);
	/// pretone may draw her pass before the tonemapper (an Unreal game): only then is the tone table made - on the
	/// present thread, the next time the effects run there. Elsewhere nothing of it exists.
	void set_tone_wanted(bool wanted);
	/// The tone table exists: her pass before the tonemapper waits for it.
	bool tone_ready();
	/// [tone] Exposure: her brightness in the game's scene, before its tone curve (1: as her engine drew her).
	float &tone_exposure();
	/// How many times her technique was drawn (ReShade's render-technique event): tells a caller of render_effects
	/// whether she was (not while the effects compile for a new target or reload, nor twice in a frame).
	unsigned long long her_runs();
	/// Where the engine says its frames are ("/dev/shm/astra-frame" or "Local\\AstraFrame"): mapped again at
	/// every hello (a restarted engine's ring is a new file). Present thread only.
	void set_frames_at(const char *where);
	/// Before a hello asking the engine for a larger ring: stop reading this one (on Windows a ring a reader still holds
	/// cannot grow; on Linux its layout changes) until the reply names it again (set_frames_at). Her picture is gone
	/// for the frame or two that takes. The same thread as set_frames_at.
	void let_go_of_frames();
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
