#!/bin/bash
# The tests that run on any machine, no game, no Windows:
#   1. the add-on's portable core (line protocol, provider state, camera math, frame ring, the tonemapper finder, the
#      tone mapping and its HLSL twin's constants) - native C++;
#   2. the AstraUE Lua mod's maths and its pipe lines, against a stub of UE4SS - plain Lua 5.4;
#   3. AstraPassthrough.fx through ReShade's own FX compiler front end (tools/fxcheck.sh), alone and with every game
#      hook (AstraGame.fxh) defined;
#   4. the HLSL ReShade makes of it through Wine's d3dcompiler_47 - what it compiles with under Proton when the game
#      ships none (tools/winecheck.sh; skipped without wine or the xwin SDK);
#   5. the WebSocket client (the link to her engine) against peers that misbehave, built for Windows and run under Wine
#      (tools/wstest.sh; skipped the same way);
#   6. the release packer (tools/gi-pack.py) refuses a game file without a digest: Astra's launcher checks every file
#      it placed beside the game's exe against its digest at each Play.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$HERE/build/tests"
mkdir -p "$OUT"
CXX=${CXX:-clang++}
# UE4SS embeds Lua 5.4: test with that version when it is installed
LUA=$(command -v lua5.4 || command -v lua || true)
LINES=""
if [ -n "$LUA" ]; then
	"$LUA" "$HERE/tests/astraue_tests.lua" "$HERE/runners/unreal/AstraUE/Scripts/main.lua" "$OUT/astraue_lines.txt"
	LINES="$OUT/astraue_lines.txt" # ...and the add-on's parser must read every line the mod wrote
else
	echo "no lua: the AstraUE tests were skipped"
fi
$CXX -std=c++20 -O1 -Wall -Wextra -pthread -I"$HERE/src" -DASTRA_FX="\"$HERE/shaders/AstraPassthrough.fx\"" -DASTRA_SRC="\"$HERE/src\"" "$HERE/tests/core_tests.cpp" "$HERE/src/lines.cpp" "$HERE/src/ring.cpp" -o "$OUT/core_tests"
"$OUT/core_tests" $LINES
"$HERE/tools/fxcheck.sh"
# ...and with every game hook defined (tests/hooks/AstraGame.fxh in place of the foundation's empty one)
mkdir -p "$OUT/hooks"
cp "$HERE/shaders/AstraPassthrough.fx" "$HERE/tests/hooks/AstraGame.fxh" "$OUT/hooks/"
"$HERE/tools/fxcheck.sh" "$OUT/hooks/AstraPassthrough.fx"
"$HERE/tools/winecheck.sh" "$HERE"/shaders/*.fx "$OUT/hooks/AstraPassthrough.fx"
"$HERE/tools/wstest.sh"
GP="$OUT/gipack"
rm -rf "$GP" && mkdir -p "$GP/staged" && printf 'x' > "$GP/staged/a.dll"
printf 'schema = 1\n[[game_files]]\nfrom = "a.dll"\nto = "${game}/a.dll"\n' > "$GP/undigested.toml"
printf 'schema = 1\n[[game_files]]\nfrom = "a.dll"\nto = "${game}/a.dll"\nsha256 = "@sha256:a.dll@"\n' > "$GP/digested.toml"
python3 "$HERE/tools/gi-pack.py" "$GP/digested.toml" "$GP/staged" "$GP/ok.zip" >/dev/null
if python3 "$HERE/tools/gi-pack.py" "$GP/undigested.toml" "$GP/staged" "$GP/bad.zip" >/dev/null 2>&1; then
	echo "gi-pack: a game file without a digest was packed"
	exit 1
fi
echo "gi-pack: a game file without a digest is refused"
