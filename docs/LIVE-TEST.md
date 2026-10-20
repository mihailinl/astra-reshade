# Live test: Astra in an Unreal game (phase 1)

Each step proves one piece and says what you should see; stop at the first step that does not show it and send
`ue4ss/UE4SS.log`, `ReShade.log` and `astra-reshade.log` (beside the game's exe).

**First run: Scarlet Skips, 2026-10-14, Linux (proton-cachyos 11.0-20261005, AMD RX 9060 XT): steps 1-3 passed**
after three fixes, all generic (see *What the first run found* at the end). Nothing in the foundation is specific
to the game.

Build once (Linux, this repo): `./build.sh && tools/package-unreal.sh` — `build/astra-gi.zip` is the release asset
Astra's game launcher installs, and `build/astra-gi/` the same files unpacked.

**Through Astra's launcher** (a user's way; README *Install*): the daemon's Games section installs the zip into the
game the user picks and starts it. **The steps below are the developer's way**: by hand, one piece at a time, with
UE4SS's `ue4ss/` beside its proxy in the game folder instead of the launcher's profile.

A zip can be checked without a game, with the launcher's own CLI over scratch folders (nothing starts; `launch
--dry-run` only prints the command): `astra-game-launcher inspect build/astra-gi.zip --sha256 <hex>`, then `plan` /
`install --yes` / `launch --dry-run --vars-stdin` / `uninstall` with `--game` a fake Steam library folder holding
`<Game>.exe` and `<Project>/Binaries/Win64/<Project>-Win64-Shipping.exe` + `<Project>/Content/Paks/x.pak`, `--exe` the
Shipping exe, `--appid` any number, `--platform linux-proton`, `--proton-prefix` a folder with an empty `user.reg`, and
`--profile` a scratch folder. Seen 2026-10-16: the six game files `add`, 21 profile files, the dry run's argv
`-applaunch <app> --ue4ss-path Z:\...\profile\ue4ss\UE4SS.dll -AstraConfig Z:\...\profile\astra-reshade.ini`, the
profile's ini `[bridge] Port = ... Token = ...`, the overrides under the Shipping exe's `AppDefaults` key; a second
Play after ReShade wrote its own files beside the exe still passes; uninstall removes the six and the overrides.

**Before anyone else builds this: push the submodule's branch first.** `extern/minecraft-in-death-stranding` is
pinned at a commit of that repo's branch `astra-reshade-fixes` (the WebSocket client and the fault guard) that may
exist only on this machine; `git clone --recursive` of this repo fails with "not our ref" until it is pushed. Death
Stranding's integration pins that repo's branch `astra-ds2-shared-guard` the same way.

## Scarlet Skips (UE 5.6, Steam app 4513480, no anti-cheat)

```bash
G="/mnt/data/SteamLibrary/steamapps/common/Scarlet Skips/ScarletSkips/Binaries/Win64"   # the REAL exe's folder
A=~/Documents/GitHub/astra-reshade/build/astra-gi
```

### 1. UE4SS loads, AstraUE reads the camera (no ReShade yet)

```bash
cp "$A/dwmapi.dll" "$G/" && cp -r "$A/ue4ss" "$G/"
```
Steam → Scarlet Skips → Properties → Launch options: `WINEDLLOVERRIDES="dwmapi=n,b" %command%`. Play, get into
the game itself (not the menu), walk and turn for 20 s, quit.

No launch options at all (what the first run used): the override as a per-exe key in the Proton prefix's
`user.reg` - Wine reads it for that exe only, and Proton's own overrides already have `dxgi=n`, so ReShade's
`dxgi.dll` beside the exe loads with no option either (proved by a launch through the root `ScarletSkips.exe` with an
empty `WINEDLLOVERRIDES`: Wine's `loaddll` trace shows both proxies `native`):
```
[Software\\Wine\\AppDefaults\\ScarletSkips-Win64-Shipping.exe\\DllOverrides] 1792000000
"dwmapi"="native,builtin"
```
(`compatdata/4513480/pfx/user.reg`; edit it only while the game is not running, wineserver rewrites it at exit.)

`$G/ue4ss/UE4SS.log` must have:
- UE4SS's banner and the engine version it detected (5.6), and no `Failed to find` / fatal line before it;
- `[AstraUE] started: every engine frame (EngineTick)` — or `EngineTick hook unavailable, polling...` (works, a
  little worse: note which);
- `[AstraUE] frame counter: the engine's` and `[AstraUE] player controller: ...`;
- every 10 s `[AstraUE] frame N  camera X Y Z  pitch .. yaw .. roll ..  fov 90.0h  player X Y Z  her X Y Z  0.0xx ms/frame`
  — the camera follows your turns, `her` stays ~1.7 m from `player`, the cost is well under 0.5 ms.

No `UE4SS.log` at all: the override is missing or `dwmapi.dll` is not beside `ScarletSkips-Win64-Shipping.exe`.

Seen 2026-10-14: `UE4SS - v3.0.1 Beta`, `[PS] Found EngineVersion: 5.6`, `[AstraUE] started: every engine frame
(EngineTick)`, `frame 839  camera 209 -72 124  pitch -3.3 yaw 160.0 roll 0.0  fov 35.0v  player 0 0 0  her -140 90 0
0.333 ms/frame`. Scarlet Skips keeps the vertical fov (`35.0v`, MaintainYFOV) and its own camera (a `BP_Camera`
view target, still while you play: a jump-rope game). The cost reads 0.3-0.9 ms: `os.clock` ticks in whole
milliseconds under Wine, so it is an average of 0s and 1s - read it over several lines.

### 2. ReShade + the add-on, and the game's depth

```bash
cp "$A/dxgi.dll" "$A/astra.addon64" "$G/" && cp -r "$A/astra-reshade" "$G/"
```
No `ReShade.ini`: ReShade loads `astra.addon64` from its own folder, and the add-on adds what her effect needs to the
`ReShade.ini` ReShade writes (README *Install*). A manual install from before 2026-10-16 (`ReShade.ini` with
`AddonPath=.\astra-reshade\`, the add-on in `astra-reshade\`) must go first (*Remove*), or ReShade keeps loading the
old add-on.
Launch options: `WINEDLLOVERRIDES="dwmapi=n,b;dxgi=n,b" %command%`. In game press **Home**:
- the overlay opens; its **Add-ons** tab lists **Astra**; the **Astra** tab says `Provider: live ... pipe: connected`
  and `Engine: waiting (is Astra running?)`;
- `$G/astra-reshade.log`: `---- Astra (astra-reshade) loaded`, on a first start `ReShade.ini: set what her effect
  needs and it lacked: EffectSearchPaths PresetPath TutorialProgress`, `pipe: serving \\.\pipe\astra-provider`,
  `pipe: a provider connected`, `provider says: AstraUE connected (UE 5.6)`.

**Depth** (the part most likely to need work on UE5): Home → Home tab → AstraPassthrough → `DebugView` = "Game
depth (1 m bands)". Expect stripes 1 m apart that sit still on walls and the floor as you move, finer near you.
- all black / all one colour / flickering: Add-ons → Generic Depth → pick the buffer with the most draw calls; try
  "Copy depth buffer before clear operations"; if still wrong, UE5 is reusing that memory before ReShade reads it:
  add `r.RDG.TransientAllocator=0` under `[SystemSettings]` in the game's `Engine.ini` (Proton prefix:
  `compatdata/4513480/pfx/drive_c/users/steamuser/AppData/Local/ScarletSkips/Saved/Config/Windows/Engine.ini`).
- stripes right but offset/squashed toward the top-left: upscaling (TSR/DLSS/FSR) draws depth smaller; set
  `Depth buffer part used` to the render scale (e.g. 0.667 0.667), or set upscaling to native/100 % for the test.
- stripes far apart near the camera: the near plane is not 10 cm; tell me.

Set `DebugView` back to "Composite".

Seen 2026-10-14 with no depth setting changed (the `ReShade.ini` the manual install shipped then, the keys the add-on now sets; native resolution, TSR at the game's
defaults): the bands sat still on the ground and the trees, finer near the camera, the sky clear; no Generic Depth
pick, no copy before clears, no `r.RDG.TransientAllocator=0`. `DebugView` can be set without the overlay:
`astra-reshade/AstraPreset.ini` = `Techniques=AstraPassthrough@AstraPassthrough.fx`, `[AstraPassthrough.fx]`,
`DebugView=2`. A step-2-only run that must not pull her off the desktop: `ASTRA_BRIDGE_PORT=25699` (nothing there).

### 3. Astra in the game

Start Astra with the companion on (she is on the desktop). Within a second or two of the game's link she leaves
the desktop (her window closes) and appears in the game, standing behind and to the right of the player.

- `astra-reshade.log`: `link: hello sent (game "ScarletSkips"...)`, `engine: {"t":"hello",...,"shm":"/dev/shm/astra-frame",...}`,
  `layer: the engine's frames are at /dev/shm/astra-frame`, `layer: connected to the engine's frames (6 slots, ...)`;
- overlay: `Engine: linked`, `cameras` and `pictures` counting up together, `late` staying small.

Look for, in order:
1. **She is there**, her feet on the floor next to the player, her size right (overlay `size` if not).
2. **Walls hide her**: walk so a pillar or wall is between the camera and her — the wall covers her, and she is
   never drawn through it.
3. **She is glued to the world on fast turns**: swing the camera quickly left and right. If she slides AHEAD of the
   turn, raise `camera delay`; if she trails it, lower it (since 0.2.5 in ms, 0..100; auto is on by default and
   picks about one provider push gap, ~17 ms at 60 Hz; before 0.2.5 it counted provider cameras, 0..3). Note the value that is right.
4. **She follows**: run, stop, turn on the spot (she stays put), run far (she catches up; past 25 m she is put
   beside you).
5. **Pause** (Esc): she disappears while the game is paused.

Seen 2026-10-14 (the owner's Astra test build, game switch on): `[switch] a game asked for her — leaving the
desktop` within 0.05 s of the hello, `layer: connected to the engine's frames (6 slots, up to 1920x1080)`, the engine
`... frames published, 0 dropped`; she stood on the grass beside the player at her size (1); the hill's terrain hid
her legs when she stood behind its crest (2); with the camera cut 40 degrees back and forth eleven times, every one
of 70 window grabs (one per 25 ms) showed her where that frame's scenery says - none a frame late, `camera delay` 1
(a count of provider cameras then, before 0.2.5's ms) is right here; swings at 70 degrees/s kept her feet on the ground (3); she followed the player 17 m (4); the game's
pause hid her and unpausing brought her back (5); quitting gave her back to the desktop (`[switch] no game holds her
— back to the desktop`). This game's camera never turns: the turns, the walk and the pause came from the test
driver `tools/ue-test-driver/` (AstraUETest, a UE4SS mod reading commands from a file), not from a player.

Expected in phase 1, not bugs: she floats on stairs and slopes (no ground traces yet), she is drawn OVER the
game's HUD and menus (the "before the UI" hook is the next phase), no shadow on the ground (the game's sun is not
read yet), the light match is approximate. HDR output must be off. Phase 2 (below) does the first three.

Without Astra's daemon, a standalone engine works too: `WGPU_PROTO_WINDOW=bridge astra-avatar-engine` (its
`BRIDGE.md`).

### What the first run found (all fixed, all generic)

1. **The game hung at start** (black, `UE4SS.log` stuck at "waiting for object construction"). ReShade loads and
   unloads its add-ons with every D3D12 device, and Unreal 5 makes and drops two probe devices first; the fault
   guard's vectored exception handler stayed registered in the unloaded add-on, and the next exception in any thread
   called it, faulted, and called it again forever (Proton log, `PROTON_LOG=1`: `call_vectored_handlers calling
   handler at 00006FFFF7AB3A90` repeating). The add-on now removes the handler at `DLL_PROCESS_DETACH`
   (`guard::uninstall`) and stops its threads when their device is destroyed, not in `DllMain`.
2. **`AstraPassthrough.fx` did not compile**: `E5017: Aborting due to not yet implemented feature: Unhandled
   attribute 'fastopt'`. Under Proton ReShade compiles with Wine's `d3dcompiler_47` (vkd3d-shader) unless the game
   ships Microsoft's (Death Stranding does; Unreal games mostly do not), and ReShade writes `[fastopt]` for every
   `[loop]`. Without the attribute that compiler unrolls the re-projection march for minutes. The loop counts now come
   from a hidden uniform and the fog texture's size; `tests/run.sh` compiles the effect with Wine's compiler
   (`tools/winecheck.sh`). First compile in game: 2.8 s, cached after.
3. **She stayed out in the pause menu**: AstraUE asked `PlayerController:IsPaused()`, plain C++ that UE4SS cannot call
   (`attempt to call a TrivialObject value`, swallowed by a `pcall`). It reads `GameplayStatics:IsGamePaused` now.

A launch from the Steam client did not reach the game's exe on this machine: explained in *Phase 2* below (a
pending update of the compatibility tool, then a Steam dialog waiting for a click).

### Phase 2 (2026-10-14 night, same game, headless gamescope while the screen was locked)

Launched with `gamescope --backend headless -W 1920 -H 1080 -- <SLR4 entry point> proton waitforexitandrun <exe>`
(screenshots `GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 gamescopectl screenshot <file>`), the owner's test-build Astra
linked as in step 3.

1. **She is drawn before the game's tonemapper.** `astra-reshade.log`:
   `pretone: the game's tonemapper found: it reads a 1920x1080 scene colour (format 26); she is drawn into it, before
   the game's tone curve, grade, bloom and UI`, then every 30 s `pretone: last 30 s: 1317 frames drawn before the
   tonemapper, 0 at presentation`. The menu's **Play** button is drawn over her and the game's grade/bloom soften her
   like the grass (phase 1: she was pasted over the button). In Scarlet Skips (UE 5.6) the tonemapper is a 3-vertex
   draw into the back buffer (R10G10B10A2) plus a second target, reading the post-TSR scene colour (R11G11B10F), bloom
   at half size and a 60x34 exposure texture. Where it is not found the log says `no tonemapper pass found in 30 s:
   she is drawn at presentation (over the game's UI)`; HDR output says so too. To see what a game's post-processing
   looks like: `ASTRA_PRETONE_TRACE=600` (frame number) logs every candidate pass of frames 600-603 (since review 13
   only where pretone is on: an Unreal game, or `ASTRA_TONE_ENGINE=unreal`).
   Her brightness in the game's light: overlay `her exposure in the game's light` (`[tone] Exposure` in the
   `-AstraConfig` ini, or `ASTRA_TONE_EXPOSURE`); `[tone] Path=present` (`ASTRA_TONE_PATH=present`) forces the old path.
   **What this run did not say: she was visibly over-exposed** - washed out, her hair and skin paler than her engine
   draws them (`shots2/cmp-before-after.png`, right). The curve inverted was a stand-in (Narkowicz's ACES fit, per
   channel in sRGB), not Unreal's: a mid grey she draws at sRGB 0.50 came out of the game at 0.558, her skin at
   about 1.0 / 0.89 / 0.84. Fixed by review 13 (below).
   `timing: last 30 s, 70 of 1373 frames got a newer game camera between her pass and Present` - her pass runs ~1 ms
   before Present; whether that ever shows as a frame of slip on fast turns is the morning's visual check.
2. **She stands on the ground under her**: `[AstraUE] frame ... player -1600 580 0  her -1314 515 45  ground 45 (325
   traces, 0 missed)  0.294 ms/frame` - 45 cm up the hill where the player stands at 0; her legs show to her feet where
   phase 1 had them under the crest. About 4 traces a second standing, up to 20 walking.
3. **The game's sun**: `[AstraUE] the game's sun: DirectionalLightComponent
   /Game/Maps/L_ScarletJump.L_ScarletJump:PersistentLevel.DirectionalLight_0.LightComponent0 (1 directional lights)`,
   sent ~5 times a second as the provider's light (direction, linear colour, in sun or not by a trace toward it); her
   engine lights her with it and the add-on casts her shadow from the engine's sun view. A shadow shape appears on the
   grass beside her in sun (`shots2/h03-sunlit.png`) - whether it is hers and where it should fall is a morning check.

**Why the Steam client launches did not reach the game** (`~/.local/share/Steam/logs/console_log.txt`,
`content_log.txt`), neither caused by UE4SS or ReShade:
- 23:25 `LaunchApp changed task to DownloadingDepots`: Steam turned the launch into an update - the game's shader
  depot and its compatibility tool's runtime (Steam Linux Runtime 4, app 4183110: `Update Required`) - and released
  the session 2 s later. No process.
- 00:17 `LaunchApp waiting for user response to ShowInterstitials`: Steam showed a dialog that needs a click (on a
  locked screen nobody answers). It sat there for an hour; at 01:18:47 it `continues with user response`, went on to
  `CreatingProcess` and failed with `AppError_16` because a directly launched copy of the game was already running.
So Astra's launcher must, before `steam://rungameid/<app>`: check that the game AND its compatibility tool are fully
installed (`appmanifest_<app>.acf` `StateFlags` 4, no update queued - the tool's appmanifest too); tell the user
when Steam may show a dialog (watch `console_log.txt` for `waiting for user response to <task>` and bring Steam's
window forward); and never launch while the game already runs (`AppError_16`). Which dialog it was is the morning's
check: launch from Steam with the screen unlocked.

### Review 13 (2026-10-15 morning, same game, headless gamescope; screenshots in `shots3/`)

An adversarial review of phase 2 found eight problems; the fixes (commits `82a86d6`, `8277b71`, `d745842`, submodule
`3b1f6a1`) and what the game showed after them:

- **Her pass before the tonemapper only in an Unreal game.** Phase 2 took it in every D3D12 game (Death Stranding, a
  Unity game with the same full-screen HDR-to-8-bit pass). Now AstraUE declares the engine (`E unreal`, provider API
  3: `astra_provider_engine`), or the launcher's ini does (`[tone] Engine=unreal`); anywhere else the add-on's
  command-stream hooks return at once. Seen: `pretone: she is drawn at presentation: no game engine declared (by the
  provider, or [tone] Engine=)` at load, then two seconds later with AstraUE `provider: the game's engine is unreal`
  and `pretone: her pass goes before the game's tonemapper when it is found: an Unreal game (declared by the
  provider)`. With the phase-2 AstraUE (no declaration) the first line stayed and every frame was at presentation
  (`0 frames drawn before the tonemapper, 837 at presentation`).
- **Her colours.** The curve inverted is now Unreal's own default (`src/tone.h`: a port of its film tonemapper with
  its blue correction and gamut expansion, inverted through a table and matrices the add-on hands the effect).
  `shots3/cmp-r13.png`: her engine's own frame, at presentation (phase 1's path, `ASTRA_TONE_PATH=present`), before
  the tonemapper with phase 2's curve, and now. She is no longer washed out: her hair keeps its gold and her skin
  its brightness as her engine draws them, with the game's UI over her (the Play and Quit buttons). What was left:
  the light match's tint from the grass around her (`Match the game's lighting`, the same on both paths; fixed since,
  *The light match on grass* below), and what the curve's port does not invert (ACES's glow and red modifier: a few hundredths on saturated reds). A game that
  changes Unreal's film settings or uses its own colour LUT is not matched; `[tone] Exposure` trims her.
- **No vanishing at the first frames.** ReShade compiles her effect for the scene colour's format the first time her
  pass runs there, and that frame has no effects at all; then the Present path is held a second and tried again.
  Seen: `pretone: her pass before the tonemapper did not draw (the effects are compiling for its 1920x1080 scene
  colour): one frame without effects; she is drawn at presentation for 60 frames, then tried again` and `last 30 s:
  1093 frames drawn before the tonemapper, 178 at presentation, 2 without effects (compiling)` on a fresh shader
  cache, `1269 ... 59 at presentation, 1 without effects` on a warm one, then `0 without effects` every 30 s.
- **Clean quit with her engine linked** (the test driver's `quit`: Unreal's own QuitGame): `the device the link
  started for is gone: its threads stopped`, the process gone 2.6-5.1 s after the command, in all five runs.
- **A 3-minute run** with the player walked 15 m and back, the camera spun and the game paused: 10 000 frames, all
  before the tonemapper after the first second, `0 tonemapper passes met Present in progress`, no fault, no Vulkan
  error in Proton's log.
- Not live-checkable here, tested offline (`tests/run.sh`, each fix broken once to see it fail): the scene colour's
  copy and her textures destroyed only 8 frames after their last use, her pass only at exactly the presented size,
  only the descriptors copied since the draw before looked up, the compute root signature put back, the WebSocket
  client's bounded waits (under Wine, against peers that never answer or never read), AstraUE's sun searched only
  when gone and its ground not snapping up to a balcony over her.

Known limit, not fixed: **a frame of her in the wrong colour space when her pass changes paths** (a hold of the
Present path begins or ends, the effects reload, pretone is switched on or off). ReShade keeps ONE uniform buffer per
effect and rewrites it on the CPU at every run of the effects; the GPU reads it when it executes that run. When a run
before the tonemapper (scene-colour values, `AstraHdr` on) and a run at Present (display values) are recorded within
the frames the GPU is behind, the earlier one may execute with the later one's uniforms - one frame of her too bright
or too dark. Fixing it needs a uniform buffer per run, which ReShade does not offer to an add-on.

Still owed: a UE5 game with dynamic resolution or an upscaler running below the output size (she is drawn at
Present there now - check it says so); a long session with alt-tab and a resolution change; a UE 4.27 D3D12 game;
fast turns and her shadow's placement looked at by a person; a launch from Steam with the screen unlocked; Death
Stranding on this foundation (its pin is still the pre-pretone `f33d736`).

### The light match on grass (2026-10-15, same game, headless gamescope)

The light match read the colour of the game's picture around her as the colour of its light; that picture is its
surfaces' colour times its light, so the park's grass painted her green (skin yellow-green, the white jacket mint).
Two guards on the estimate, effect uniforms:
- `Light colour: largest shift` (`LightTintMax`, 0.1): how far the light's colour may move any of her channels from
  neutral (0.5: no limit but the old clamp);
- `Light colour: ignore the scene's own cast` (`LightSceneCast`, 1): the cast the whole picture shares (its mean) is
  not light where the colour around her agrees with it - never past it and never against it, so a grey path in a
  green park does not turn her magenta, and a lamp of another colour stays.

The look before them is `LightTintMax=0.5` and `LightSceneCast=0` under `[AstraPassthrough.fx]` in
`astra-reshade/AstraPreset.ini`. Seen, in shots of the same moment with the options switched every 400 ms (a test
build of the effect): on the grass her skin pink and her jacket white again; on the dirt path, the picture around
her still mostly green park, neither magenta nor grey.

The same mistake in brightness: sunlit grass read as bright light and brightened her by about 1.09, clipping her pale
skin and white jacket. `Match the game's lighting: largest brightening` (`LightGainMax`, 1: never brighter than her
engine draws her; 1.3 is the look before) caps only that side; the darkening is never capped. Seen, same method: the
share of her pixels with a channel at full brightness, on the grass 26 % at 1.3, 19.5 % at 1.1, 1.9 % at 1.0 (0.8 %
with no light match at all); on the dirt path 15 %, 7 %, 1.6 % (1.6 %); in a tree's shade her legs darkened the
same at all three (picture brightness 0.69-0.70, 0.77 with no light match).

### Her picture sharp and steady at 2560x1440 (2026-10-16 night, same game, headless gamescope at 2560x1440)

What the owner saw at 2560x1440 - her outline jittering with the camera still, white pixels at her edges, her picture
soft and stair-stepped - had five causes, all generic:
- **Jitter**: ReShade has ONE constant buffer per effect, rewritten on the CPU every run; her pass before the
  tonemapper is recorded into the game's command list and runs on the GPU after the NEXT frame's write, so it drew
  picture N with crop rectangle N+1 (seen in a test build: the uniforms were one run ahead of the picture in 577 of
  600 frames). Every per-frame value now goes through a 16x1 texture uploaded with the picture (`src/frame_meta.h`).
- **White fringe**: before the tonemapper her coverage was blended in the scene's linear light, where her white is
  ~10x a mid grey past the curve's steep top - a 5 % covered pixel showed 0.81 where her engine draws 0.53. Her edge
  is now blended in display space through the curve's model.
- **Stairs**: her colour was filtered up from 1920x1080 but her depth point-sampled, cutting the soft edge along
  whole texels (1.33-pixel steps). Her depth is now the nearest of the colour's bilinear footprint.
- **Her shoes flickering**: TSR jitters the game's depth every frame and the grass at her feet covered a texel one
  frame and not the next. Her visibility is now the share of nine depth texels she is in front of.
- **Soft**: the add-on capped her picture at 1080p worth of pixels and the engine's ring was a fixed 1920x1080: her
  picture was scaled up x1.33. The hello now names the size it needs (`"want":[2560,1440]`, `src/hello_rules.h`; up
  to the engine's 3840x2160) and says hello again when the window grows past the ring (the old ring let go first).

Measured with the effect and add-on of `fix/ue-look` against the 0.2.0 ones the launcher installs, the engine of
the `want` change (lean release, scratch state, port 25614), the main menu (camera still, she idles):
- **Her picture's size**: 0.2.0 sends no `want`: reply `maxW 1920, maxH 1080`, her picture 1920x1080 shown x1.33.
  Now: `hello sent (... want 2560x1440 ...)`, reply `maxW 2560, maxH 1440`, `[bridge] frame ring ... (2560x1440
  max)`, and all 600 pictures read from the ring 2560x1440 (crop 1216-1280 px wide): her pixels are the game's.
- **Jumps** (her torso patch frame to frame, 600 frames at 60 Hz, sub-pixel phase correlation; a jump = 0.8 px or
  more and straight back): 0.2.0 52 of 599; now 0, 0, 1 and 1 in four captures at 2560x1440, 1 and 0 with the
  ring pinned to 1920x1080 (`WGPU_BRIDGE_MAX`) - a single one is her own animation reversing.
- **Fringe** (two frozen pictures each: the engine stopped, the screen and the ring's picture read; her edge pixels
  left of the menu's buttons and above the grass, a = 0.02..0.6; a fringe pixel is brighter by more than 24 levels
  than her own colour - through the game's mapping fitted on her covered pixels - mixed with the background in
  display space): 0.2.0 434 and 473 of 1450 and 1727 edge pixels (27-30 %; 183 and 273 by more than 64); now at
  2560x1440 7 and 4 of 1377 and 1055 (0.4-0.5 %, none by 64); now with the ring pinned to 1920x1080 16 and 34 (none
  by 64). Contact sheets: `fringe-sheet-1.png`, `fringe-sheet-2.png` (rows 0.2.0, fix at 1080, fix at 1440; each
  window as shown | fringe in red) and `sharpness-sheet.png`, in `/mnt/data/scratch-ap/ue-look2/`.
- **Repeated pictures** (her patch exactly still from one frame to the next, the box's load average 27-49 from other
  builds): 0.2.0 9.7 %; in one game session with only the engine restarted, ring pinned 1920x1080 17.2 % and 12.2 %
  (52.6-52.7 pictures/s), at 2560x1440 22.5 %, 20.2 % and 28.2 % (49.6, 51.7 and, under the heaviest load, 36.5
  pictures/s; the engine drew a newer camera instead of 0-30 of 600).

Still open:
- **The colour LUT**: her pass maps her picture into the scene through a model of Unreal's default tone curve
  (`tone.h`); this game's own curve has a brighter toe (shown 0.1 -> 0.175, 0.2 -> 0.295, 0.5 -> 0.597; gain 1.77
  in the dark to 1.2 in the bright), so she is paler and flatter than her engine draws her. The game's curve should be
  read off the game (its LUT), not modelled.
- **Repeated pictures**: about 10 % at 1080, more at 2560x1440 and under load. Two causes seen: frames whose
  provider camera never reached the engine (two provider cameras between presents), and pictures that come after
  the 6 ms wait (`WaitMs`) - a 2560x1440 picture is 1.8x the engine's drawing and readback. Not measured yet: a
  quiet box, a longer wait, the game's own frame pacing.
- **Not seen live**: the hello again after a window grew (gamescope's size is fixed; the rule is tested in
  `tests/core_tests.cpp`), and native Windows (where the ring is let go before that hello).
- **A refused add-on retries every frame**: an engine that refuses games (its crash marker, see `BRIDGE.md`) got
  ~1300 hellos a minute; the add-on should back off after an `error` reply. (Testers: stopping the engine within its
  grace after the game leaves keeps the marker, and the next engine refuses every game once.)

### Remove

```bash
cd "$G" && rm -rf dwmapi.dll dxgi.dll astra.addon64 ReShade.ini astra-reshade ue4ss astra-reshade.log ReShade.log
```
and clear the launch options (or delete the `AppDefaults\ScarletSkips-Win64-Shipping.exe\DllOverrides` key from
the prefix's `user.reg`, with the game closed).

## S.T.A.L.K.E.R. 2 (UE 5.1)

**Not installed now** (only its shader caches are left in `/mnt/data/SteamLibrary`): reinstall it (~175 GB)
first. Same steps; the exe folder is `Stalker2/Binaries/Win64/` (beside `Stalker2-Win64-Shipping.exe`), the Proton
prefix is `compatdata/1643320`. Differences to expect:
- it is first person: her spot (behind the player) is behind the camera. Turn around to see her, or put her in
  front: in `ue4ss/Mods/AstraUE/Scripts/main.lua` set `M.BACK = -200.0` and restart the game;
- it runs DLSS/FSR/TSR by default: step 2's depth check matters most here (render scale for `Depth buffer part used`);
- a large world: watch for `link: origin ...` lines (her floating origin moving) — she must not jump when one appears.
- its own camera manager subclass: AstraUE reads the final camera cache, so this should not matter; if her
  position is off only while aiming down sights, the FOV changes per frame — note it.

## Likely failure modes

| symptom | cause | what to try |
|---|---|---|
| no `UE4SS.log` | proxy not loaded | `dwmapi=n,b` in launch options; `dwmapi.dll` beside the `-Shipping.exe` |
| UE4SS log stops after scanning / game crashes at start | UE4SS's signature scan does not match this 5.6 build | send the log; a newer RE-UE4SS experimental build, or a custom game config from UE4SS's `zCustomGameConfigs` |
| `[AstraUE] error (n): ...` | a UE call failed in this game | send the line |
| no Add-ons tab in ReShade | a ReShade without add-on support | `dxgi.dll` must be ReShade 6.8.0 *Addon* (digest in `astra-gi.toml`) |
| `pipe: cannot serve` | another game holds the pipe | close it |
| `Provider: silent` but the pipe is connected | the game is in a menu or loading | get into gameplay |
| she is offset sideways/vertically, or the wrong size | FOV axis or letterboxing | send the `[AstraUE] frame` line and a screenshot |
| she swims on fast turns | wrong camera delay | the slider, step 3.3 |
| `late` grows fast | her picture arrives too late | `wait for her picture` up to 8–10 ms; check the engine's own FPS |
| the game's Vulkan mode (`-vulkan`) | ReShade's dxgi.dll does not hook Vulkan | run the game in DX12 (the default) |
