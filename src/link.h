#pragma once
// The link to the Astra avatar engine's game bridge (BRIDGE.md): a WebSocket on 127.0.0.1. Once per
// presented frame it sends her placement and the game's NEWEST camera (the provider's), and tells the
// compositor which camera is being PRESENTED (the provider's camera `camera_delay` writes back: a game with a
// render thread presents a frame its game thread has already moved past).
//
// Coordinates: the provider's (glTF world, metres) minus a floating origin near her, so the engine's f32
// stays precise in a world kilometres wide. Cameras sent are remembered by id (64 of them): the compositor
// re-projects a picture from the camera it was drawn with, looked up here.
#include "view.h"
#include <string>

namespace enginelink
{
	void start(int port, const std::string &token);
	/// FreeLibrary only (at process exit the link thread is already gone).
	void stop();
	/// Once per presented frame, before the effects (the present event). backbuffer: the game's picture size.
	void frame(int backbuffer_w, int backbuffer_h);
	bool connected();

	/// The camera (absolute world) a picture was drawn with, the projection aspect it was drawn at, and the
	/// floating origin then (what the engine calls world in that frame = absolute - origin).
	bool pose_of(unsigned long long cam_id, view::Pose &out, float &aspect, double origin[3]);
	/// How far she moved (world) since the camera `cam_id` was sent: an older picture of her moves along.
	bool moved_since(unsigned long long cam_id, double out[3]);

	/// Frames the presented camera lags the provider's newest (the overlay's slider, saved in ReShade.ini).
	int &camera_delay();
	/// Her size on top of the provider's (the overlay's slider).
	float &size();
	/// Show her at all (the overlay's checkbox).
	bool &enabled();

	struct Stats
	{
		unsigned long long sent, received, rebases, cams;
		bool answered; // the engine's hello came on this connection
		bool provider_fresh, provider_visible;
		long long camera_age_ms;
	};
	Stats stats();
	/// The engine's last message, for the overlay.
	std::string last_event();
}
