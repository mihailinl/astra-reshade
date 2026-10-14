# AstraUETest — a live-test driver (not shipped)

A UE4SS Lua mod that turns the camera, moves the player and pauses the game from commands written into a file,
so `docs/LIVE-TEST.md` step 3 (her glued on fast turns, following, hidden on pause) runs without hands on the
keyboard — and in games whose own camera never turns (Scarlet Skips). Generic Unreal API only.

Install beside AstraUE for the test, remove after:

```bash
cp -r tools/ue-test-driver/AstraUETest "$G/ue4ss/Mods/"        # loaded through its enabled.txt
printf 'swing 147 -8 60 1 8\n' > "$G/ue4ss/Mods/AstraUETest/cmd.txt"
rm -rf "$G/ue4ss/Mods/AstraUETest"
```

The commands are listed at the top of `AstraUETest/Scripts/main.lua`; each answers with an `[AstraUETest]` line
in `UE4SS.log`.
