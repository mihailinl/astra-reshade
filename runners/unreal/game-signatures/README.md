# Per-game UE4SS signature overrides

UE4SS's built-in pattern scan finds the engine's core addresses (GUObjectArray, GMalloc,
`StaticConstructObject_Internal`, `ConsoleManagerSingleton`, ...) automatically in almost every Unreal game.
When a game's compiler/build settings make the built-in pattern ambiguous, a `UE4SS_Signatures/<Symbol>.lua`
file can override the scan for just that symbol
(<https://docs.ue4ss.com/guides/fixing-compatibility-problems.html>,
<https://docs.ue4ss.com/dev/custom-game-configs.html>).

## Why this directory exists, and why it is not in `../ue4ss-overlay/`

`../ue4ss-overlay/` is the runner's shared tree: `tools/package-unreal.sh` copies it into `${game}/ue4ss/`
(beside the game's exe, UE4SS 3.x's own layout - astra-gi.toml's own top comment explains why it lives there
and not the profile) for **every** game `astra-gi.toml` installs onto (`exe = "*"`). Anything placed there
reaches every Unreal game this add-on supports.

A `UE4SS_Signatures/<Symbol>.lua` file does not *add* a candidate alongside UE4SS's own scan - it
**replaces** that scan outright. From UE4SS's own source
(`UE4SS/src/Signatures.cpp::setup_lua_scan_overrides`, RE-UE4SS experimental 3.0.1-1164-g5e627997):

```cpp
// If any Lua scripts are found, add overrides so that the Lua script can perform the aob scan
// instead of the Unreal API itself
setup_lua_scan_overrides(m_working_directory, config);
```

and inside `scan_from_lua_script`, the signature list handed to the scanner is built from **only** the
Lua file's own pattern:

```cpp
signature_containers.emplace_back(SignatureContainer{{
                                                             {lua.get_string().data()},
                                                     }, /* ... */});
```

So: the override activates the instant the **file exists** (`std::filesystem::exists(...)`), regardless
of what its `Register()`/`OnMatchFound()` do at runtime - a game-aware check inside the Lua script cannot
opt a game out, because the C++ side never asks the script whether it should apply. If the AOB then finds
nothing in *that* game's exe, UE4SS does **not** fall back to its built-in scan: it records
`"Was unable to find AOB for '<Symbol>' via Lua script"` as the scan's error for that symbol, the same way
a failed built-in scan would - and losing `StaticConstructObject_Internal` or `GUObjectHashTables` is
fatal to UE4SS (the "Fatal Error: PS scan timed out" behaviour GTA III DE hit before this override existed
is exactly that failure mode).

**Consequence:** a GTA-III-specific byte pattern dropped into the shared `ue4ss-overlay/` would turn every
*other* Unreal game this runner serves from "scans fine on its own" into "UE4SS fails outright" - its exact
bytes are extremely unlikely to occur at the right call site in an unrelated game's binary. The override
must only ever reach GTA III DE's own `ue4ss/UE4SS_Signatures/`, never the shared runner's.

## The shape that ships it safely - no launcher change needed

`astra-game-launcher`'s manifest (`src/manifest.rs`) has no conditional-file concept: `[[files]]` and
`[[game_files]]` entries are unconditional once a package installs. But `[target].exe` does not have to be
`"*"` - when a manifest names an exact exe (and, better, a `steam_appid`), `plan_install`
(`src/plan.rs::plan_install`) refuses to install it on any other game (`GAME_MISMATCH`), exactly the way it
already refuses a mismatched Steam appid. That is the safety property this override needs, and the launcher
already has it.

So GTA III DE gets its **own** manifest, `../astra-gi-gta3.toml`: `exe =
"Gameface/Binaries/Win64/LibertyCity.exe"`, `steam_appid = 1546970`, otherwise the same build as
`astra-gi.toml`, plus two extra `[[game_files]]` entries that place
`UE4SS_Signatures/{StaticConstructObject,ConsoleManager}.lua` from this directory into
`${game}/ue4ss/UE4SS_Signatures/` - beside the `UE4SS.dll` this package also ships there. A player picks it
from the launcher's list the same way they'd pick the generic runner - it just will not install (or run)
anywhere but GTA III DE.

This is why the two Lua files sit under `GTA3-DefinitiveEdition/` here rather than directly under
`UE4SS_Signatures/`: this directory can hold a sibling folder per game that needs an override, each wired
into its own exe-pinned manifest, without any of them ever touching the shared runner.

### If a future game needs this AND the launcher owner wants one shared manifest

The exe-pinned-sibling-manifest shape above needs zero changes and is what ships today. If a later game
also needs an override and the near-duplication between sibling manifests (see the comment at the top of
`astra-gi-gta3.toml`) becomes the bigger cost, the clean alternative is an optional per-rule condition on
`[[files]]`/`[[game_files]]` in `manifest.rs` - e.g. `when_exe = "Gameface/Binaries/Win64/LibertyCity.exe"`
(or a list), defaulting to "always" so every existing manifest keeps meaning what it means today. That
would let one `astra-gi.toml` carry several games' overrides as data rows instead of several whole sibling
files. Not needed yet - flagging it only so the next one isn't decided twice.

## Vice City / San Andreas - The Definitive Edition

Not installed on this machine (their `ViceCity.exe` / `SanAndreas.exe` were not available to scan), so
nothing here is claimed to apply to them. All three DE titles share "Gameface" (modified UE 4.26), so their
binaries may well share these exact call sites and byte patterns - but that is a guess, not evidence, and
this override stays scoped to GTA III DE's own exe (and its own manifest) until someone actually scans
those two exes and proves it (or disproves it: a differently-compiled DLC/game-specific class layout would
move these offsets).

## The two files here

- `GTA3-DefinitiveEdition/UE4SS_Signatures/StaticConstructObject.lua` - resolves the ambiguity between
  the old multi-argument overload (`0x140C89270`) and the canonical
  `StaticConstructObject_Internal(const FStaticConstructObjectParameters&)` (`0x141A2B330`, the one UE4SS
  wants). Evidence and the exact AOB are in the file's own header comment.
- `GTA3-DefinitiveEdition/UE4SS_Signatures/ConsoleManager.lua` - resolves the ambiguity between the real
  `ConsoleManagerSingleton` lazy initializer (`0x1416EB630`, confirmed by its own call into **GMalloc at
  the exact address UE4SS.log already reports**, `0x14540F8D0`) and a different per-cvar accessor function
  that merely calls into it (`0x1426853B0`). Evidence and the exact AOB are in the file's own header
  comment.

Both AOBs were verified to match **exactly once** across the whole `LibertyCity.exe` image (every section
`objdump -h` lists - `.text`/`.rdata`/`.data`/`.pdata`/`.msvcjmc`/`_RDATA`/`.rsrc`/`.reloc`/`.rkstr`), at
exactly the address each file names.
