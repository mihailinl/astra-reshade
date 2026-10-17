#!/bin/bash
# tests/relink_tests.cpp - does the link to Astra's engine, and the provider pipe, come back after the device
# that started them is destroyed and a new one takes its place (DS2/Decima's start-up reload; alt-tab and
# fullscreen toggles elsewhere)? Drives the real link.cpp/pipe.cpp/ws.cpp (tests/layer_stub.cpp stands in for
# layer.cpp's GPU resources, which the link never touches) - built for Windows (clang-cl, the xwin CRT/SDK as
# build.sh) and run under Wine, in tools/winecheck.sh's prefix. Without wine or xwin it says so and passes.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
X=${XWIN:-$HOME/.xwin}
MC=${MC_DIR:-$HERE/extern/minecraft-in-death-stranding}
command -v wine >/dev/null || { echo "relinktest: no wine, skipped"; exit 0; }
[ -d "$X/crt/include" ] || { echo "relinktest: no xwin CRT/SDK at $X, skipped"; exit 0; }
OUT="$HERE/build/relinktest"
mkdir -p "$OUT"
(cd "$OUT" && clang-cl --target=x86_64-pc-windows-msvc /nologo /O1 /MT /EHsc /std:c++20 /DNOMINMAX /DWIN32_LEAN_AND_MEAN -fuse-ld=lld-link \
	-Wno-unused-command-line-argument /imsvc "$X/crt/include" /imsvc "$X/sdk/include/ucrt" /imsvc "$X/sdk/include/um" /imsvc "$X/sdk/include/shared" \
	/I "$HERE/src" /I "$HERE/include" /I "$HERE/tests" /I "$MC/ds/src" \
	"$HERE/tests/relink_tests.cpp" "$HERE/tests/layer_stub.cpp" "$HERE/src/link.cpp" "$HERE/src/provider.cpp" "$HERE/src/pipe.cpp" "$HERE/src/lines.cpp" \
	"$MC/ds/src/ws.cpp" /Fe"relink_tests.exe" \
	/link /libpath:"$X/crt/lib/x86_64" /libpath:"$X/sdk/lib/um/x86_64" /libpath:"$X/sdk/lib/ucrt/x86_64" >/dev/null
	rm -f ./*.obj)
export WINEPREFIX="$HERE/build/winecheck/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
[ -d "$WINEPREFIX" ] || wineboot -i >/dev/null 2>&1
wine "$OUT/relink_tests.exe"
