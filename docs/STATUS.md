# Status — where the Unreal runner stands (2026-10-24)

Astra installs the **latest stable** GitHub release of this repo (`astra-gi.zip`, and `astra-gi-gta3.zip`
for GTA III DE). Promoting a prerelease is the release step, and no Astra update is needed for it.

| | tag | state |
|---|---|---|
| stable (what players get) | `v0.2.1` | — |
| candidate | `v0.2.11` (main `1483712`) | **prerelease**, waiting for the owner's play-test |
| superseded candidate | `v0.2.10` (`bd61248`) | prerelease; 0.2.11 contains all of it |

**Promote** after the owner's «ок» on the 4-step test (Trello "Avatar engine — агенты", card "Unreal-раннер 0.2.11"):
`gh release edit v0.2.11 -R mihailinl/astra-reshade --prerelease=false --latest`. If only the UI step fails
and walking is fine, promote `v0.2.10` instead.

## What 0.2.10 / 0.2.11 add over 0.2.1

- 0.2.5–0.2.10 (AstraUE Lua mod):
  - she walks collision-aware: a capsule sweep per step, a slide along walls, steps, never onto crates you are not on;
  - she follows the navmesh where the level has one;
  - she is sized by the measured projection (`ProjectWorldLocationToScreen`), not the reported FOV;
  - the engine's near plane is honoured;
  - camera age is in ms, and she faces you in first person;
  - ReShade's generic depth is seeded (`UseAspectRatioHeuristics=0`, `FilterFormat=6`) for games that render below the screen's resolution.
- 0.2.11 (the add-on): her pass runs before the game's tonemapper on **Direct3D 11** too (state captured and restored
  with the context's own Get*/Set*), and for a tonemapper **below the presented size** (Unreal's screen percentage,
  e.g. Poppy Playtime 768x432 in 768x436 at 2560x1440). Only the main view's tonemapper is used: its scene colour must
  be on the depth buffer's grid. Retries after a miss are held by the clock.

## Known open

- D3D11 deferred contexts are not drawn into: she stays at Present for those frames, which means over the UI.
- A menu with no 3D scene (Poppy's main menu) binds no depth, so she stays at Present there. Nothing of her shows.
- Her picture is sampled down without filtering when the game renders far below the screen. Possible shimmer.
- The live kit used for 0.2.11 (headless Poppy, restore + diff, xdotool into gamescope's own display only):
  `/mnt/data/scratch-ap/d3d11/live/run{2..6}.sh` on the owner's PC.
