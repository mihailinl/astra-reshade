# astra-reshade

> **Experimental — phase 2.** First run in a game on 2026-10-14: Scarlet Skips (UE 5.6) under Proton, Astra in the game with its depth, glued on camera cuts, hidden on pause; then drawn before the game's tonemapper (its grade, bloom and UI over her), standing on the ground under her, lit by the game's sun — what was seen and what it took is in [docs/LIVE-TEST.md](docs/LIVE-TEST.md).


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

**Before the tonemapper.** ReShade runs effects at Present, over the game's UI and past its tone curve.
`src/pretone.cpp` finds the game's tonemapper in its Direct3D 12 command stream - a full-screen draw into an 8/10-bit
target of the presented size that reads an HDR texture of that size (`src/passfind.h`; what a draw reads comes from
the descriptors the game copied for it) - and runs the effects there, into a copy of that scene colour, copied back,
the game's state put back after. Her 8-bit picture is mapped into the scene's light by an inverted filmic curve
(`src/tone.h`; `[tone] Exposure` in the `-AstraConfig` ini, the overlay's "her exposure"). Not found, HDR output,
another API or `[tone] Path=present`: at Present as before; the log and the overlay say which.

## What is in here

| path | what |
|---|---|
| `src/` | the add-on (`astra.addon64`): the provider API (`provider.cpp`), its pipe (`pipe.cpp`), the engine link (`link.cpp`), the frame ring reader and compositor (`layer.cpp`, `ring.cpp`) |
| `include/astra_provider.h` | the provider API: a C ABI and the pipe's line protocol |
| `shaders/AstraPassthrough.fx` | the compositing effect |
| `runners/unreal/` | the Unreal runner: the AstraUE UE4SS mod, ReShade.ini, the UE4SS mod list, the `astra-gi.toml` manifest draft |
| `tests/` | `tests/run.sh`: the core in native C++, AstraUE against a stub of UE4SS, the effect through ReShade's own FX compiler |
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

A game whose integration needs more of the compositor (the game's UI laid back over her, another mod's world in front
of her) ships its own `AstraGame.fxh` beside `AstraPassthrough.fx`, in place of the foundation's empty one: the hooks
are listed at the top of the effect.

## Install by hand (until Astra's launcher does it)

Everything goes into the folder of the game's REAL executable (Unreal: `<Project>/Binaries/Win64/`, beside
`<Project>-Win64-Shipping.exe`), see `docs/LIVE-TEST.md` for the exact steps:

- ReShade 6.8.0 **with full add-on support**: its `ReShade64.dll` as `dxgi.dll`, and `runners/unreal/ReShade.ini`;
- `astra-reshade/astra.addon64` and `astra-reshade/Shaders/{AstraPassthrough.fx, AstraGame.fxh, ReShade.fxh}`;
- for Unreal: UE4SS (`dwmapi.dll` + `ue4ss/`) with `runners/unreal/AstraUE` in `ue4ss/Mods/` and our `mods.txt`.

Under Proton, the game's launch options: `WINEDLLOVERRIDES="dwmapi=n,b;dxgi=n,b" %command%`.

The add-on logs to `astra-reshade.log` beside the game's exe (or beside the `-AstraConfig=` ini). ReShade's overlay
(Home) has an **Astra** tab: the engine link, the provider, "camera delay" and "wait for her picture".

The engine's port and token come from `-AstraConfig=<ini>` on the game's command line (`[bridge] Port=`, `Token=`
— Astra's launcher writes it), else `ASTRA_BRIDGE_PORT` / `ASTRA_BRIDGE_TOKEN`, else 25600 with no token.

## The Unreal runner (`runners/unreal/`)

`astra-gi.toml` (schema 1 of `astra-bepinex/docs/GAME-INTEGRATION-MANIFEST.md`) installs, for any UE game:
UE4SS whole into the launcher's profile (loaded from there by its proxy's `--ue4ss-path`), and into the game's exe
folder only the two proxies (`dwmapi.dll`, `dxgi.dll`), `ReShade.ini`, the add-on and its two shader files.
Upstream binaries are digest-pinned; `tools/package-unreal.sh` fills in our files' digests and checks them all.

What the launcher needs that schema 1 does not say yet:

1. **`${game}` for `engine = "unreal"` must be the REAL exe's folder** — `<Project>/Binaries/Win64/` (the
   `*-Win64-Shipping.exe`), not the root folder with the small launcher stub the user may pick. Both proxies must
   sit beside the process that loads them.
2. **Our own files in the game folder change every release** (the add-on, the .fx): as `[[game_files]]` each update
   asks the user again. ReShade can only find them relative to its `ReShade.ini`, which must sit in the game
   folder; a `${profile}` placeholder expanded inside a game file (a rendered `ReShade.ini` with
   `AddonPath=<profile>`) would let them live in the profile as `[[files]]`.
3. **Files the tools write themselves**: ReShade rewrites `ReShade.ini` and creates `AstraPreset.ini` and its
   cache; UE4SS writes its log and cache in the profile. The ledger should know a file it placed may change
   (no digest, `ReShade.ini`) and what to remove that it did not place (`astra-reshade/AstraPreset.ini`).
4. **A rolling upstream**: UE 5.6+ needs RE-UE4SS's experimental line, whose release tag is replaced at every
   upstream commit; the zip must be mirrored as our own release asset (MIT allows it) for the pinned URL to live.
5. **An existing ReShade or UE4SS** in the game folder (a user's own ReShade without add-on support, or another
   UE4SS) is "another mod loader": the launcher should say so, not replace it.

## Licence

MIT (`LICENSE`). The runner's zip also carries RE-UE4SS (MIT), ReShade (BSD-3-Clause) and ReShade.fxh (CC0-1.0)
unmodified, each with its licence in `LICENSES/`.
