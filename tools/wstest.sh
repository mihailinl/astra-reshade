#!/bin/bash
# tests/ws_tests.cpp - the WebSocket client against peers that misbehave - built for Windows (clang-cl, the xwin
# CRT/SDK as build.sh) and run under Wine, in tools/winecheck.sh's prefix. Without wine or xwin it says so and passes.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
X=${XWIN:-$HOME/.xwin}
MC=${MC_DIR:-$HERE/extern/minecraft-in-death-stranding}
command -v wine >/dev/null || { echo "wstest: no wine, skipped"; exit 0; }
[ -d "$X/crt/include" ] || { echo "wstest: no xwin CRT/SDK at $X, skipped"; exit 0; }
OUT="$HERE/build/wstest"
mkdir -p "$OUT"
(cd "$OUT" && clang-cl --target=x86_64-pc-windows-msvc /nologo /O1 /MT /EHsc /std:c++20 /DNOMINMAX /DWIN32_LEAN_AND_MEAN -fuse-ld=lld-link \
	-Wno-unused-command-line-argument /imsvc "$X/crt/include" /imsvc "$X/sdk/include/ucrt" /imsvc "$X/sdk/include/um" /imsvc "$X/sdk/include/shared" \
	/I "$MC/ds/src" "$HERE/tests/ws_tests.cpp" "$MC/ds/src/ws.cpp" /Fe"ws_tests.exe" \
	/link /libpath:"$X/crt/lib/x86_64" /libpath:"$X/sdk/lib/um/x86_64" /libpath:"$X/sdk/lib/ucrt/x86_64" >/dev/null
	rm -f ./*.obj)
export WINEPREFIX="$HERE/build/winecheck/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
[ -d "$WINEPREFIX" ] || wineboot -i >/dev/null 2>&1
wine "$OUT/ws_tests.exe"
