# astra-reshade

> **Experimental — phase 2.** First run in a game on 2026-10-14: Scarlet Skips (UE 5.6) under Proton, Astra in the game with its depth, glued on camera cuts, hidden on pause; then drawn before the game's tonemapper (its grade, bloom and UI over her), standing on the ground under her, lit by the game's sun — what was seen and what it took is in [docs/LIVE-TEST.md](docs/LIVE-TEST.md). Phase 2 as first run washed her out (a stand-in tone curve, not Unreal's); review 13's fixes (Unreal's own curve, pretone only in Unreal games, thread and lifetime races) are in [docs/LIVE-TEST.md](docs/LIVE-TEST.md) *Review 13*.

> **Before pushing this repo, push its submodule's branch.** `extern/minecraft-in-death-stranding` is pinned at a
> commit of that repo's branch `astra-reshade-fixes` that may exist only locally; a clone of this repo fails with
> "not our ref" until it is pushed. (Death Stranding's integration pins the branch `astra-ds2-shared-guard` the same way.)


Astra, the Astra companion's 3D avatar, inside **any D3D11/D3D12 game ReShade
runs in** — on Windows, and on Linux under Proton/vkd3d. Her own engine (the Astra avatar engine, a separate
native program) draws her from the game's camera; this ReShade add-on puts that picture into the game's picture
**with depth**: the game's walls hide her, she stands on its ground, her light and fog follow the game's.

The add-on knows nothing about any game. A small **provider** mod in the same process tells it, every frame,
where the game's camera is and where she stands. The first provider is **AstraUE**, a UE4SS Lua mod that does
this for any Unreal Engine 4/5 game (`runners/unreal/`).

```
the game's process (Proton or Windows)                       Astra avatar engine (native, game bridge)
  UE4SS ─ AstraUE (Lua)  camera + player, every frame
             │  \\.\pipe\astra-provider  (or the C API, include/astra_provider.h)
  ReShade ─ astra.addon64
             link.cpp   ─── WebSocket 127.0.0.1:25600 ──────>  hello, avatar (her feet, pace), cam (the camera)
                        <──────────────────────────────────    hello (where its frames are)
             layer.cpp  <── shared memory "MCPT" ───────────   her picture + linear depth (her rectangle only)
  AstraPassthrough.fx   re-projects her picture onto the presented camera, depth-tests it against the game
  pretone.cpp           ...right before the game's tonemapper, into its HDR scene colour (else at Present)
```

**Before the tonemapper — Unreal games only.** ReShade runs effects at Present, over the game's UI and past its tone
curve. In a game whose provider declares it Unreal (`astra_provider_engine("unreal")`, AstraUE's `E unreal` line) or
whose launcher ini says `[tone] Engine=unreal`, `src/pretone.cpp` finds the game's tonemapper in its Direct3D 11 or 12
command stream - a full-screen draw into an 8/10-bit target of exactly the presented size that reads an HDR texture
of that size (`src/passfind.h`; what a draw reads: on D3D12 the descriptors the game copied for it since the draw
before, on D3D11 what the context says is bound when a full-screen draw comes) - and runs the effects there, on the
game's recording thread (never while Present is inside ReShade), into a copy of that scene colour, copied back, the
game's state put back after (tracked and replayed on D3D12; read with Get* and written back with Set* on D3D11,
whose context hands its own state back on request - a deferred D3D11 context is not drawn into yet). Her 8-bit
picture is mapped into the scene's
light by inverting **Unreal's default** tonemapping (`src/tone.h`: its film curve, desaturations, blue correction and
gamut expansion; a game's own film settings or colour LUT are not read - `[tone] Exposure` in the `-AstraConfig` ini,
the overlay's "her exposure", trims her). Every other game - Death Stranding, a Unity game, a provider that reads its
camera at presentation - and no tonemapper found, HDR output, another API or `[tone] Path=present`: at Present,
exactly as before pretone (its command-stream hooks return at once). Not quite "as before" in one case: a frame whose
pass at the tonemapper could not draw because ReShade was still compiling the effects for the scene colour has NO
effects at all (ReShade has marked the frame's effects drawn); after such a frame the Present path is held for a
while (1 s, then longer, given up after 8 tries), so it costs a frame or a few, not a stretch. And a frame where
the path changes may show her once in the wrong colour space (ReShade's one uniform buffer per effect: LIVE-TEST.md,
*Review 13*, known limit). The log and the overlay say which path is used and why (`pretone: ...`).

## What is in here

| path | what |
|---|---|
| `src/` | the add-on (`astra.addon64`): the provider API (`provider.cpp`), its pipe (`pipe.cpp`), the engine link (`link.cpp`), the frame ring reader and compositor (`layer.cpp`, `ring.cpp`), its settings (`config.cpp`, the rules in `config_rules.h`) |
| `include/astra_provider.h` | the provider API: a C ABI and the pipe's line protocol |
| `shaders/AstraPassthrough.fx` | the compositing effect |
| `runners/unreal/` | the Unreal runner: the AstraUE UE4SS mod, the UE4SS mod list, the `astra-gi.toml` manifest Astra's game launcher installs |
| `tests/` | `tests/run.sh`: the core in native C++ (incl. the pass finder and pretone's rules over recorded sequences, the tone mapping through a port of Unreal's tonemapper), AstraUE against a stub of UE4SS, the effect through ReShade's own FX compiler and Wine's d3dcompiler, the WebSocket client under Wine |
| `docs/LIVE-TEST.md` | how to try it in a real game (Scarlet Skips, S.T.A.L.K.E.R. 2) and what proves each piece |

The add-on was lifted from the (private) Death Stranding add-on — its game-agnostic half: the frame-ring reader,
the re-projection with its timing fixes (the camera read at the START of Present, a bounded wait for the picture
of the camera being presented, an older picture moved along with her), the depth test and the fog/light match.
The WebSocket client and the fault guard are [Minecraft in Death Stranding](https://github.com/mihailinl/minecraft-in-death-stranding)'s,
used as a git submodule.

## Build (Linux)

```bash
git clone --recursive <this repo> && cd astra-reshade
extern/minecraft-in-death-stranding/tools/fetch_deps.sh   # ReShade 6.8.0 headers + DLL, ReShade.fxh, imgui
./build.sh                                                # -> build/astra.addon64 (clang-cl + lld-link + xwin)
tests/run.sh                                              # no game, no Windows needed
tools/package-unreal.sh                                   # -> build/astra-gi.zip (the Unreal runner's release asset)
```

`build.sh` needs `clang-cl`, `lld-link` and the MSVC CRT + Windows SDK from [xwin](https://github.com/Jake-Shadle/xwin)
in `~/.xwin` (or `XWIN=`).

AstraUE (the Unreal provider) also traces the ground under her (`LineTraceSingle` down, 4-20 times a second) and
reads the game's sun (its main `DirectionalLightComponent`), sent as `astra_provider_light`.

## The provider API

A provider calls these once per game frame, from any thread (the add-on reads them at presentation behind
seqlocks — a writer never blocks it, it never sees half a write). Coordinates are **glTF's** (right-handed, +Y up,
metres), the game's absolute world position; the add-on keeps a floating origin near her.

```c
astra_provider_avatar(feet, fwd, up, vel, speed_m_s, airborne, scale);   // where she stands (first)
astra_provider_camera(frame, eye, fwd, up, fov_deg, fov_is_horizontal, aspect, near_m);   // the game's final camera
astra_provider_visible(0);   // menus, loading, pause: she is hidden (a silent provider hides her too, after 0.3 s)
astra_provider_light(toward_sun, color, intensity, visible);   // optional: the game's sun (her light, her shadow)
astra_provider_cue("play", "seat");   // optional: her animation set's states/triggers, like Astra's daemon
```

Native code finds them with `GetModuleHandleW(L"astra.addon64")` + `GetProcAddress`. Code that cannot call native
functions writes the same calls as text lines to `\\.\pipe\astra-provider` (`include/astra_provider.h`, PIPE
PROTOCOL) — AstraUE does, from UE4SS's Lua (`io.open(pipe, "r+b")`, one write per frame).

Her speed and `airborne` are raw facts: what they mean (walk, run, a jump) is her animation set's business.

Version 2 (`astra_provider_version() >= 2`; look each one up by name, an older add-on has none):

```c
astra_provider_camera2(frame, eye, fwd, up, fov_deg, fov_is_horizontal, aspect, near_m, far_m); // a depth buffer with a far plane
astra_provider_anchor("hips");          // her pos names her pelvis (a seat), not her feet; "feet" or NULL: back
astra_provider_param("seated", 1.0f);   // more raw facts for her animation set (sticky; NaN: back to its default)
astra_provider_log("...");              // a line in astra-reshade.log
astra_provider_on_present(fn, ctx);     // a provider that reads the camera from memory at presentation: fn runs at
                                        // the start of every Present, and the camera it writes IS the presented one
```

Version 3:

```c
astra_provider_engine("unreal");        // the game's engine, a raw fact: in an Unreal game she is drawn before its
                                        // tonemapper (pipe line "E unreal"); anything else, or nothing: at Present
```

A game whose integration needs more of the compositor (the game's UI laid back over her, another mod's world in front
of her) ships its own `AstraGame.fxh` beside `AstraPassthrough.fx`, in place of the foundation's empty one: the hooks
are listed at the top of the effect.

## Install

### Through Astra's launcher (how a user gets it)

`tools/package-unreal.sh` builds `build/astra-gi.zip`, the release asset Astra's game launcher
(`astra-game-launcher`, driven by the Astra daemon's Games section) installs into any Unreal game the user picks:

- into the launcher's **profile** for that game: UE4SS whole, with `AstraUE` in its `Mods/` and our `mods.txt`;
- beside the game's **real** exe (`<Project>/Binaries/Win64/`, the `*-Win64-Shipping.exe` the launcher detects, never
  the root launcher stub): `dwmapi.dll` (UE4SS's proxy), `dxgi.dll` (ReShade 6.8.0 Addon), `astra.addon64` and
  `astra-reshade/Shaders/{AstraPassthrough.fx, AstraGame.fxh, ReShade.fxh}` — every one digest-pinned in the manifest;
- at every Play: the engine's port and this session's token into `<profile>/astra-reshade.ini` (`[bridge] Port=`,
  `Token=`), the Proton overrides `dwmapi`/`dxgi` = `native,builtin` for the Shipping exe in the prefix's `user.reg`,
  and the launch `steam -applaunch <app> --ue4ss-path <profile>\ue4ss\UE4SS.dll -AstraConfig <profile>\astra-reshade.ini`
  (Windows paths, `Z:\...` under Proton; the token is never on the command line).

**No `ReShade.ini` ships**: ReShade rewrites it while the game runs, and the launcher checks every file it placed
against its digest at each Play. ReShade's defaults load the add-on (with no `AddonPath`, add-ons are loaded from
ReShade's own folder, so `astra.addon64` sits beside `dxgi.dll`), and the add-on sets at load, only where missing,
what her effect needs and ReShade's defaults lack (`src/config_rules.h`): `.\astra-reshade\Shaders\**` added to
`EffectSearchPaths`, `PresetPath=.\astra-reshade\AstraPreset.ini`, `TutorialProgress=4`. It does so while ReShade loads
add-ons at the game's first device, before the effect runtime reads its config at the swap chain; ReShade then writes
the file itself. Seen 2026-10-16 under Wine 11.19 (D3D12 through vkd3d, a hidden-window test program, no game): no
`ReShade.ini` → `Loading add-on from '...\astra.addon64'`, `Successfully compiled '...\astra-reshade\Shaders\AstraPassthrough.fx'`,
and a `ReShade.ini` written with the three keys; a second start sets nothing; a person's own `EffectSearchPaths` and
`PresetPath` are kept (ours appended). The technique needs no preset: it is `enabled = true`.

Uninstall removes what the launcher placed and puts back the Proton overrides; what ReShade wrote itself
(`ReShade.ini`, `ReShade.log`, `astra-reshade/AstraPreset.ini`) stays, inert without `dxgi.dll`.

### By hand (the developer path)

Lay `build/astra-gi/` out the same way, see `docs/LIVE-TEST.md` for the exact steps: `dwmapi.dll`, `dxgi.dll`,
`astra.addon64` and `astra-reshade/` beside the game's real exe, and for Unreal UE4SS's `ue4ss/` folder there too
(UE4SS finds it beside its proxy with no `--ue4ss-path`). Under Proton, the game's launch options:
`WINEDLLOVERRIDES="dwmapi=n,b;dxgi=n,b" %command%`. A manual install from before 2026-10-16 (a `ReShade.ini` with
`AddonPath=.\astra-reshade\`, the add-on inside `astra-reshade\`) must be removed first, or ReShade keeps loading the
old add-on from there.

The add-on logs to `astra-reshade.log` beside the game's exe (or beside the `-AstraConfig` ini). ReShade's overlay
(Home) has an **Astra** tab: the engine link, the provider, "camera delay" and "wait for her picture".

The engine's port and token come from the first of: the ini named by `-AstraConfig <path>` (or `-AstraConfig=<path>`)
on the game's command line (`[bridge] Port=`, `Token=` — Astra's launcher writes it; by hand it is the developer's
override); `ASTRA_BRIDGE_PORT` / `ASTRA_BRIDGE_TOKEN`; `ReShade.ini`'s `[ASTRA] Port=` / `Token=`; else 25600 with no
token. The first source that is there decides both; a named ini that is missing or unreadable is skipped and the
load line says so. The token is never logged, only whether there is one. `ReShade.ini`'s other `[ASTRA]` keys are the
overlay's sliders, as before.

## The Unreal runner (`runners/unreal/`)

`astra-gi.toml` (schema 1 of `astra-bepinex/docs/GAME-INTEGRATION-MANIFEST.md`, as `astra-game-launcher` reads it)
installs what *Through Astra's launcher* lists. Upstream binaries are digest-pinned; `tools/package-unreal.sh` fills in
our files' digests, checks them all, and refuses a game file without one (`tools/gi-pack.py`).

Still open on the launcher's side:

1. **Our own files in the game folder change every release** (the add-on, the .fx): as `[[game_files]]` each update
   asks the user again.
2. **Files the tools write themselves** stay after uninstall: ReShade's `ReShade.ini`, `ReShade.log` and
   `astra-reshade/AstraPreset.ini` beside the exe (inert without `dxgi.dll`); UE4SS's log and cache are in the
   profile, which uninstall removes.
3. **A rolling upstream**: UE 5.6+ needs RE-UE4SS's experimental line, whose release tag is replaced at every
   upstream commit; the zip must be mirrored as our own release asset (MIT allows it) for the pinned URL to live.
4. **An existing ReShade or UE4SS** in the game folder: a foreign `dxgi.dll` is refused as another mod loader; a
   foreign `dwmapi.dll` (another UE4SS) is not on the launcher's list and would be backed up and replaced.

## Licence

MIT (`LICENSE`). The runner's zip also carries RE-UE4SS (MIT), ReShade (BSD-3-Clause) and ReShade.fxh (CC0-1.0)
unmodified, each with its licence in `LICENSES/`.
