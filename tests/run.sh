#!/bin/bash
# The tests that run on any machine, no game, no Windows:
#   1. the add-on's portable core (line protocol, provider state, camera math, frame ring) - native C++;
#   2. the AstraUE Lua mod's maths and its pipe lines, against a stub of UE4SS - plain Lua 5.4;
#   3. AstraPassthrough.fx through ReShade's own FX compiler front end (tools/fxcheck.sh).
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
$CXX -std=c++20 -O1 -Wall -Wextra -pthread -I"$HERE/src" "$HERE/tests/core_tests.cpp" "$HERE/src/lines.cpp" "$HERE/src/ring.cpp" -o "$OUT/core_tests"
"$OUT/core_tests" $LINES
"$HERE/tools/fxcheck.sh"
