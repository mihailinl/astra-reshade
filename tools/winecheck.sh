#!/bin/bash
# The effect through Wine's own d3dcompiler_47 (vkd3d-shader's HLSL compiler) - what ReShade compiles with under
# Proton when the game ships no d3dcompiler_47.dll (most Unreal games ship none; Death Stranding does, which hid
# this). ReShade's front end writes the HLSL of each entry point (tools/fxcheck --hlsl), tools/hlslc.exe compiles it.
#   tools/winecheck.sh [file.fx ...]        (default: shaders/*.fx)
#   D3DCOMPILER=<a d3dcompiler_47.dll>      compile with that one instead (Microsoft's: Windows' compiler and timings)
# Needs wine and the xwin CRT/SDK (as build.sh); without them it says so and passes.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
X=${XWIN:-$HOME/.xwin}
command -v wine >/dev/null || { echo "winecheck: no wine, skipped"; exit 0; }
[ -d "$X/crt/include" ] || { echo "winecheck: no xwin CRT/SDK at $X, skipped"; exit 0; }
OUT="$HERE/build/winecheck"
mkdir -p "$OUT"
"$HERE/tools/fxcheck.sh" /dev/null >/dev/null 2>&1 || true # builds fxcheck (the check of /dev/null itself fails)
FXCHECK="$HERE/build/reshade-fx-6.8.0/fxcheck"
SHADERS="$HERE/extern/minecraft-in-death-stranding/third_party/shaders"
if [ ! -f "$OUT/hlslc.exe" ] || [ "$HERE/tools/hlslc.cpp" -nt "$OUT/hlslc.exe" ]; then
	(cd "$OUT" && clang-cl --target=x86_64-pc-windows-msvc /nologo /O1 /MT /EHsc /D_CRT_SECURE_NO_WARNINGS -fuse-ld=lld-link -Wno-unused-command-line-argument \
		/imsvc "$X/crt/include" /imsvc "$X/sdk/include/ucrt" /imsvc "$X/sdk/include/um" /imsvc "$X/sdk/include/shared" \
		"$HERE/tools/hlslc.cpp" /Fe"hlslc.exe" /link /libpath:"$X/crt/lib/x86_64" /libpath:"$X/sdk/lib/um/x86_64" /libpath:"$X/sdk/lib/ucrt/x86_64" >/dev/null
	rm -f "$OUT"/*.obj)
fi
rm -f "$OUT/d3dcompiler_47.dll"
[ -z "$D3DCOMPILER" ] || cp "$D3DCOMPILER" "$OUT/d3dcompiler_47.dll"
export WINEPREFIX="$OUT/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;d3dcompiler_47=n,b"
[ -d "$WINEPREFIX" ] || wineboot -i >/dev/null 2>&1
[ $# -gt 0 ] || set -- "$HERE"/shaders/*.fx
status=0
for fx in "$@"; do
	H="$OUT/hlsl/$(basename "$fx" .fx)"
	rm -rf "$H" && mkdir -p "$H"
	"$FXCHECK" --hlsl "$H" "$fx" "$SHADERS" >/dev/null
	files=()
	for f in "$H"/*.hlsl; do files+=("Z:$f"); done
	echo "winecheck: $fx (${D3DCOMPILER:-the d3dcompiler_47 of Wine})"
	wine "$OUT/hlslc.exe" "${files[@]}" || status=1
done
exit $status
