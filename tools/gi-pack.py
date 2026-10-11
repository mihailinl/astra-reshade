#!/usr/bin/env python3
"""Packs a game integration's release asset, astra-gi.zip, from a staged folder and its manifest.

    tools/gi-pack.py <astra-gi.toml> <staged folder> <out.zip>

The manifest (schema 1, astra-bepinex/docs/GAME-INTEGRATION-MANIFEST.md) may say "@sha256:<path in the zip>@"
where a digest of OUR file belongs (it changes with every build): it is filled in from the staged file. Then every
file the manifest names must be staged, and every digest it pins must match - an upstream file (ReShade's dxgi.dll)
that is not the release it pins stops the build here, not at a player's install. The zip holds the filled manifest
and the whole staged folder (licences included: the launcher extracts only what the manifest names, a reader of the
zip sees them all), in a fixed order with fixed timestamps: the same files give the same zip, byte for byte.
"""
import hashlib
import os
import re
import sys
import tomllib
import zipfile


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def staged_digest(staged, rel):
    p = os.path.join(staged, rel)
    if not os.path.isfile(p):
        sys.exit("not staged: " + rel)
    return digest(p)


def main(manifest, staged, out):
    text = open(manifest, encoding="utf-8").read()
    text = re.sub(r"@sha256:([^@]+)@", lambda m: staged_digest(staged, m.group(1)), text)
    m = tomllib.loads(text)
    for f in m.get("files", []):
        base = f["from"].removesuffix("**").rstrip("/")
        if not os.path.exists(os.path.join(staged, base)):
            sys.exit("not staged: " + f["from"])
    for f in m.get("game_files", []):
        p = os.path.join(staged, f["from"])
        if not os.path.isfile(p):
            sys.exit("not staged: " + f["from"])
        if "sha256" in f and digest(p) != f["sha256"]:
            sys.exit("digest mismatch: %s is not the file the manifest pins" % f["from"])
    entries = [("astra-gi.toml", text.encode("utf-8"))]
    for d, dirs, files in os.walk(staged):
        dirs.sort()
        for name in sorted(files):
            p = os.path.join(d, name)
            rel = os.path.relpath(p, staged).replace(os.sep, "/")
            if rel == "astra-gi.toml" or os.path.islink(p):
                continue
            with open(p, "rb") as f:
                entries.append((rel, f.read()))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for rel, data in sorted(entries):
            info = zipfile.ZipInfo(rel, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    pinned = sum("sha256" in f for f in m.get("game_files", []))
    print("%s: %d [[files]], %d [[game_files]] (%d pinned), sha256 %s" % (out, len(m.get("files", [])), len(m.get("game_files", [])), pinned, digest(out)))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
