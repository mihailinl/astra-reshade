#!/bin/bash
# Cross-build the add-on on Linux: clang-cl + lld-link against the MSVC CRT / Windows SDK from xwin.
#   ./build.sh    ->  build/astra.addon64
# Shared code (the WebSocket client, fault containment) comes from Minecraft in Death Stranding, a git submodule:
#   git submodule update --init && extern/minecraft-in-death-stranding/tools/fetch_deps.sh   (ReShade's headers, imgui)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
X=${XWIN:-$HOME/.xwin}
MC=${MC_DIR:-$HERE/extern/minecraft-in-death-stranding} # MC_DIR: a local checkout instead
TP="$MC/third_party"
[ -f "$MC/ds/src/ws.cpp" ] || { echo "the submodule is missing: git submodule update --init"; exit 1; }
[ -f "$TP/reshade-include/reshade.hpp" ] || { echo "run $MC/tools/fetch_deps.sh first"; exit 1; }
[ -d "$X/crt/include" ] || { echo "no MSVC CRT / Windows SDK at $X (xwin splat --output $X, or set XWIN)"; exit 1; }
mkdir -p "$HERE/build"
cat > "$HERE/compile_flags.txt" <<FLAGS
--driver-mode=cl
--target=x86_64-pc-windows-msvc
/std:c++20
/EHsc
/DNOMINMAX
/TP
-imsvc$X/crt/include
-imsvc$X/sdk/include/ucrt
-imsvc$X/sdk/include/um
-imsvc$X/sdk/include/shared
-I$(cd "$TP" && pwd)/reshade-include
-I$(cd "$TP" && pwd)/imgui
-I$HERE/include
-I$MC/ds/src
FLAGS
cd "$HERE/build"
clang-cl --target=x86_64-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc /MT /LD /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN -fuse-ld=lld-link \
	-Wno-unused-command-line-argument -Wno-missing-braces \
	/imsvc "$X/crt/include" /imsvc "$X/sdk/include/ucrt" /imsvc "$X/sdk/include/um" /imsvc "$X/sdk/include/shared" \
	/I "$TP/reshade-include" /I "$TP/imgui" /I "$HERE/include" /I "$HERE/src" /I "$MC/ds/src" \
	"$HERE"/src/*.cpp "$MC"/ds/src/{guard,ws}.cpp /Fe"astra.addon64" \
	/link /libpath:"$X/crt/lib/x86_64" /libpath:"$X/sdk/lib/um/x86_64" /libpath:"$X/sdk/lib/ucrt/x86_64"
rm -f ./*.obj ./*.lib ./*.exp
ls -la astra.addon64
