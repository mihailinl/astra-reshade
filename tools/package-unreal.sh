#!/bin/bash
# Builds the Unreal runner's release asset: build/astra-gi.zip (astra-gi.toml + every file it names), from
#   our build (./build.sh -> build/astra.addon64), shaders/, runners/unreal/ (AstraUE, ReShade.ini, the manifest);
#   UE4SS (downloaded, digest-checked), ReShade 6.8.0 Addon + ReShade.fxh (the submodule's fetch_deps.sh).
# Fills the manifest's @sha256:<path>@ with our files' digests and checks every upstream digest it pins.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
TP="$HERE/extern/minecraft-in-death-stranding/third_party"
CACHE="$HERE/build/cache"
OUT="$HERE/build/astra-gi"
# RE-UE4SS's experimental line (UE 5.6-5.8). experimental-latest is a ROLLING tag: its asset is replaced at every
# upstream commit, so this URL dies with the next one - mirror the zip (MIT) as an asset of our own release.
UE4SS_URL=${UE4SS_URL:-https://github.com/UE4SS-RE/RE-UE4SS/releases/download/experimental-latest/UE4SS_v3.0.1-1164-g5e627997.zip}
UE4SS_SHA256=3c72142a383e25e9f9553ac9e4d5677aa572e98cb369cac57200efb2e381e603
RESHADE_DLL_SHA256=0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7

sha() { sha256sum "$1" | cut -d' ' -f1; }
need() { [ -f "$1" ] || { echo "missing $1 ($2)"; exit 1; }; }
need "$HERE/build/astra.addon64" "run ./build.sh"
need "$TP/reshade-6.8.0-addon/ReShade64.dll" "run extern/minecraft-in-death-stranding/tools/fetch_deps.sh"
need "$TP/shaders/ReShade.fxh" "run extern/minecraft-in-death-stranding/tools/fetch_deps.sh"
[ "$(sha "$TP/reshade-6.8.0-addon/ReShade64.dll")" = "$RESHADE_DLL_SHA256" ] || { echo "ReShade64.dll is not 6.8.0 Addon's"; exit 1; }

mkdir -p "$CACHE"
zip_file="$CACHE/$(basename "$UE4SS_URL")"
[ -f "$zip_file" ] || curl -fsSL -o "$zip_file" "$UE4SS_URL" || { echo "UE4SS download failed (rotated? set UE4SS_URL to a mirror)"; exit 1; }
[ "$(sha "$zip_file")" = "$UE4SS_SHA256" ] || { echo "UE4SS zip digest mismatch"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/astra-reshade/Shaders" "$OUT/LICENSES"
python3 - "$zip_file" "$OUT" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    if i.filename.startswith("/") or ".." in i.filename.split("/") or (i.external_attr >> 16) & 0o170000 == 0o120000:
        sys.exit("unsafe entry in the UE4SS zip: " + i.filename)
z.extractall(sys.argv[2])
PY
cp -r "$HERE/runners/unreal/ue4ss-overlay/." "$OUT/ue4ss/"
cp -r "$HERE/runners/unreal/AstraUE" "$OUT/ue4ss/Mods/AstraUE"
cp "$TP/reshade-6.8.0-addon/ReShade64.dll" "$OUT/dxgi.dll"
cp "$HERE/runners/unreal/ReShade.ini" "$OUT/ReShade.ini"
cp "$HERE/build/astra.addon64" "$OUT/astra-reshade/astra.addon64"
cp "$HERE/shaders/AstraPassthrough.fx" "$HERE/shaders/AstraGame.fxh" "$TP/shaders/ReShade.fxh" "$OUT/astra-reshade/Shaders/"
cp "$HERE/LICENSE" "$OUT/LICENSES/astra-reshade-MIT.txt"
cp "$OUT/ue4ss/LICENSE" "$OUT/LICENSES/RE-UE4SS-MIT.txt"
# the WebSocket client and the fault guard compiled into astra.addon64 come from this MIT repository (and its own lineage)
cp "$HERE/extern/minecraft-in-death-stranding/LICENSE" "$OUT/LICENSES/minecraft-in-death-stranding-MIT.txt"
cp "$HERE/extern/minecraft-in-death-stranding/mc/LICENSE.universal-modder" "$OUT/LICENSES/universal-modder-MIT.txt"
[ -f "$CACHE/ReShade-LICENSE.md" ] || curl -fsSL -o "$CACHE/ReShade-LICENSE.md" https://raw.githubusercontent.com/crosire/reshade/v6.8.0/LICENSE.md
cp "$CACHE/ReShade-LICENSE.md" "$OUT/LICENSES/ReShade-BSD-3-Clause.md"

# the manifest: our digests in, every file it names staged and matching what it pins; then the zip (reproducible)
rm -f "$HERE/build/astra-gi.zip"
python3 "$HERE/tools/gi-pack.py" "$HERE/runners/unreal/astra-gi.toml" "$OUT" "$HERE/build/astra-gi.zip"
echo "build/astra-gi.zip  $(du -h "$HERE/build/astra-gi.zip" | cut -f1)"
