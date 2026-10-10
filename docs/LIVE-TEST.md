# Live test: Astra in an Unreal game (phase 1)

Nothing here has run in a game yet. Each step proves one piece and says what you should see; stop at the first
step that does not show it and send `UE4SS.log` and `astra-reshade.log` (both beside the game's exe).

Build once (Linux, this repo): `./build.sh && tools/package-unreal.sh` — `build/astra-gi/` is then the whole
install, laid out as the launcher would place it.

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

`$G/ue4ss/UE4SS.log` must have:
- UE4SS's banner and the engine version it detected (5.6), and no `Failed to find` / fatal line before it;
- `[AstraUE] started: every engine frame (EngineTick)` — or `EngineTick hook unavailable, polling...` (works, a
  little worse: note which);
- `[AstraUE] frame counter: the engine's` and `[AstraUE] player controller: ...`;
- every 10 s `[AstraUE] frame N  camera X Y Z  pitch .. yaw .. roll ..  fov 90.0h  player X Y Z  her X Y Z  0.0xx ms/frame`
  — the camera follows your turns, `her` stays ~1.7 m from `player`, the cost is well under 0.5 ms.

No `UE4SS.log` at all: the override is missing or `dwmapi.dll` is not beside `ScarletSkips-Win64-Shipping.exe`.

### 2. ReShade + the add-on, and the game's depth

```bash
cp "$A/dxgi.dll" "$A/ReShade.ini" "$G/" && cp -r "$A/astra-reshade" "$G/"
```
Launch options: `WINEDLLOVERRIDES="dwmapi=n,b;dxgi=n,b" %command%`. In game press **Home**:
- the overlay opens; its **Add-ons** tab lists **Astra**; the **Astra** tab says `Provider: live ... pipe: connected`
  and `Engine: waiting (is Astra running?)`;
- `$G/astra-reshade.log`: `---- Astra (astra-reshade) loaded`, `pipe: serving \\.\pipe\astra-provider`,
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
   turn, raise `camera delay`; if she trails it, lower it (0..3; 1 is the default). Note the value that is right.
4. **She follows**: run, stop, turn on the spot (she stays put), run far (she catches up; past 25 m she is put
   beside you).
5. **Pause** (Esc): she disappears while the game is paused.

Expected in phase 1, not bugs: she floats on stairs and slopes (no ground traces yet), she is drawn OVER the
game's HUD and menus (the "before the UI" hook is the next phase), no shadow on the ground (the game's sun is not
read yet), the light match is approximate. HDR output must be off.

Without Astra's daemon, a standalone engine works too: `WGPU_PROTO_WINDOW=bridge astra-avatar-engine` (its
`BRIDGE.md`).

### Remove

```bash
cd "$G" && rm -rf dwmapi.dll dxgi.dll ReShade.ini astra-reshade ue4ss astra-reshade.log ReShade.log
```
and clear the launch options.

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
