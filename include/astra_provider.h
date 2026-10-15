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
 *   C <frame> <px> <py> <pz> <fx> <fy> <fz> <ux> <uy> <uz> <fov> <v|h> <aspect> <near> [<far>]
 *                                                     astra_provider_camera (with <far>: astra_provider_camera2)
 *   A <px> <py> <pz> <fx> <fy> <fz> <ux> <uy> <uz> <vx> <vy> <vz> <speed> <0|1> <scale>    astra_provider_avatar
 *   N <point>                                                                             astra_provider_anchor
 *   K <name> <value|null>                                                                 astra_provider_param
 *   V <0|1>                                                                               astra_provider_visible
 *   L <tx> <ty> <tz> <r> <g> <b> <intensity> <visible>                                    astra_provider_light
 *   P <state> | T <token> | R                                                             astra_provider_cue
 *   I <integration> <game name ...>                                                       astra_provider_identity
 *   E <engine>                                                                            astra_provider_engine
 *   M <text>                                                                              astra_provider_log
 * A frame's lines may come in one write ("A ...\nC ...\n"); send the avatar BEFORE the camera.
 * astra_provider_on_present has no line: a pipe cannot call back.
 */
#ifndef ASTRA_PROVIDER_H
#define ASTRA_PROVIDER_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1: camera, avatar, visible, light, cue, identity.
 * 2: + camera2 (the far plane), anchor, param, log, on_present.
 * 3: + engine. Look a newer function up by name: an older add-on has none (GetProcAddress gives NULL). */
#define ASTRA_PROVIDER_VERSION 3

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

/* The same with the game's far clip plane (metres) - for a game whose depth buffer (reversed Z) has one: the
 * add-on linearises the game's depth with near AND far. far_m 0 = none (an infinite projection, as
 * astra_provider_camera assumes). Version 2. */
ASTRA_PROVIDER_API void astra_provider_camera2(unsigned long long frame, const double pos[3], const float fwd[3], const float up[3],
	float fov_deg, int fov_is_horizontal, float aspect, float near_m, float far_m);

/* Where she stands this frame: pos = her FEET; fwd her facing; up (may be NULL: she stays upright);
 * vel her velocity m/s (may be NULL); speed her ground speed m/s (a raw fact - her animation set decides
 * walk/run); airborne: off the ground (debounce it); scale: her size, 0 = 1. */
ASTRA_PROVIDER_API void astra_provider_avatar(const double pos[3], const float fwd[3], const float up[3], const float vel[3], float speed,
	int airborne, float scale);

/* Which point of her `pos` names (astra_provider_avatar): "feet" (the default), or a VRM humanoid bone - "hips"
 * with the seat point puts her pelvis on a seat. Kept until changed; NULL or "" = "feet". Version 2. */
ASTRA_PROVIDER_API void astra_provider_anchor(const char *point);

/* One more animator parameter for her animation set, beside speed and airborne: a RAW fact (a number; a bool
 * as 1/0), never a decision - what it means is her set's call. name: [A-Za-z_][A-Za-z0-9_]*, <= 32 characters.
 * Kept and sent with every placement until changed; a NaN value hands it back to the set's default. At most 16
 * (more are refused). Version 2. */
ASTRA_PROVIDER_API void astra_provider_param(const char *name, float value);

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

/* The game's engine, a raw fact: "unreal" (any Unreal 4 or 5 game). The add-on decides what it allows: in a game
 * declared Unreal, Astra is drawn BEFORE the game's tonemapper when it finds it (the game's grade, bloom and UI then
 * apply to her; her colours are mapped through Unreal's default tone curve) - anywhere else, and without this call,
 * she is drawn at presentation, over the finished picture. Names: [A-Za-z0-9_.-], <= 64; kept until changed, NULL or
 * "" = none. Version 3. */
ASTRA_PROVIDER_API void astra_provider_engine(const char *engine);

/* A line for the add-on's log (astra-reshade.log, beside Astra's settings for this game): a provider's own
 * diagnostics in the same file. Rate-limited (its first 200 lines, then every 100th). Version 2. */
ASTRA_PROVIDER_API void astra_provider_log(const char *line);

/* For a provider that reads the game's camera from memory at presentation (no hook on the game's own frame):
 * `callback(context)` is called on the present thread at the start of every Present, before the add-on reads
 * anything - so a camera written inside it IS the frame being presented (the add-on's camera delay does not
 * apply to it). One provider at a time: a second registration replaces the first. NULL unregisters - do it
 * before your module unloads. Contain your own faults in it (the add-on does not wrap it: fault guards of two
 * modules must not nest). Version 2. */
ASTRA_PROVIDER_API void astra_provider_on_present(void (*callback)(void *context), void *context);

#ifdef __cplusplus
}
#endif

#endif
