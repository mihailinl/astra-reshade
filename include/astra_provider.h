/* astra_provider.h - the in-process PROVIDER API of the astra-reshade add-on (astra.addon64).
 *
 * The add-on composites Astra into a game's picture; it knows nothing about the game. A PROVIDER - a mod
 * living in the same process (a UE4SS mod, a Unity plugin, a script) - tells it, once per game frame,
 * where the game's camera is and where she stands. The add-on does the rest: the link to her engine,
 * her picture from its frame ring, re-projection onto the presented camera, the depth test.
 *
 * Two ways in, one API:
 *   1. Native: GetModuleHandleW(L"astra.addon64") + GetProcAddress for the functions below (C ABI,
 *      x64 calling convention). The add-on is loaded by ReShade when the game's swap chain is made,
 *      which can be after your mod starts: look it up again until it is there.
 *   2. A named pipe, for providers that cannot call native code (UE4SS Lua): one text line per call,
 *      see PIPE PROTOCOL below. The pipe server calls exactly these functions.
 *
 * Every function may be called from any thread (the game thread, usually). The add-on reads at
 * presentation; a writer never blocks it and it never sees half a write (seqlocks). Calls are cheap:
 * a copy into a slot.
 *
 * COORDINATES: glTF's - right-handed, +Y up, METRES, the game's ABSOLUTE world position (double).
 * The add-on keeps a floating origin near her, so kilometres-wide worlds lose no precision.
 * (Unreal: gl = (x, z, y) * 0.01 from UE's (x forward, y right, z up) centimetres.)
 *
 * PIPE PROTOCOL (\\.\pipe\astra-provider, byte mode, ASCII lines ending in '\n', fields split by spaces):
 *   C <frame> <px> <py> <pz> <fx> <fy> <fz> <ux> <uy> <uz> <fov> <v|h> <aspect> <near>   astra_provider_camera
 *   A <px> <py> <pz> <fx> <fy> <fz> <ux> <uy> <uz> <vx> <vy> <vz> <speed> <0|1> <scale>    astra_provider_avatar
 *   V <0|1>                                                                               astra_provider_visible
 *   L <tx> <ty> <tz> <r> <g> <b> <intensity> <visible>                                    astra_provider_light
 *   P <state> | T <token> | R                                                             astra_provider_cue
 *   I <integration> <game name ...>                                                       astra_provider_identity
 *   M <text>                                                                              a line for astra-reshade.log
 * A frame's lines may come in one write ("A ...\nC ...\n"); send the avatar BEFORE the camera.
 */
#ifndef ASTRA_PROVIDER_H
#define ASTRA_PROVIDER_H

#ifdef __cplusplus
extern "C" {
#endif

#define ASTRA_PROVIDER_VERSION 1

#ifdef ASTRA_PROVIDER_EXPORTS
#define ASTRA_PROVIDER_API __declspec(dllexport)
#else
#define ASTRA_PROVIDER_API
#endif

/* ASTRA_PROVIDER_VERSION of the add-on that is loaded. */
ASTRA_PROVIDER_API int astra_provider_version(void);

/* The game's camera for one game frame - the FINAL view the game renders this frame with.
 *   frame   your frame counter, increasing (a jump back is taken as a restart of the provider)
 *   pos     the eye; fwd: where it looks (unit); up: its up (unit, not parallel to fwd)
 *   fov_deg the field of view in degrees; fov_is_horizontal: 1 if fov_deg spans the width (Unreal's
 *           default), 0 if it spans the height
 *   aspect  the width/height that fov refers to; 0 = the game's back buffer's
 *   near_m  the game's near clip plane in metres (its depth buffer is linearised with it); 0 = 0.1 */
ASTRA_PROVIDER_API void astra_provider_camera(unsigned long long frame, const double pos[3], const float fwd[3], const float up[3],
	float fov_deg, int fov_is_horizontal, float aspect, float near_m);

/* Where she stands this frame: pos = her FEET; fwd her facing; up (may be NULL: she stays upright);
 * vel her velocity m/s (may be NULL); speed her ground speed m/s (a raw fact - her animation set decides
 * walk/run); airborne: off the ground (debounce it); scale: her size, 0 = 1. */
ASTRA_PROVIDER_API void astra_provider_avatar(const double pos[3], const float fwd[3], const float up[3], const float vel[3], float speed,
	int airborne, float scale);

/* 0: she is not shown (a menu, a loading screen, the game paused). Default 1. A provider that stops
 * sending cameras hides her too (after ~0.3 s). */
ASTRA_PROVIDER_API void astra_provider_visible(int visible);

/* The game's sun (or moon): toward it (unit, world), its colour (linear), intensity relative to a clear
 * noon (1), and how much of her is in it (0..1). Sent to her engine ~10 times a second. */
ASTRA_PROVIDER_API void astra_provider_light(const float toward[3], const float color[3], float intensity, float visible);

/* An animation cue, the same ones Astra's daemon sends: kind "play" (enter <name> - a state of her
 * animation set), "trigger" (raise token <name>), "resume" (name ignored). Names: [A-Za-z0-9_.-], <= 64. */
ASTRA_PROVIDER_API void astra_provider_cue(const char *kind, const char *name);

/* Who is providing (shown to Astra's owner; optional): an integration id ("astra.unreal") and the game's
 * display name. Before the first camera, or the add-on reconnects to say it. */
ASTRA_PROVIDER_API void astra_provider_identity(const char *integration, const char *game);

#ifdef __cplusplus
}
#endif

#endif
