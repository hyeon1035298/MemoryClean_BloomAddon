# SPDX-License-Identifier: GPL-3.0-only
"""Adds files of the add-on's own to the addon.json PillTaskbar's tools wrote.

    python add-files.py [--settings] <bundle folder> <file name>...

Each name is a file in the bundle folder. Its SHA-256 goes into "files" (replacing an
older entry of that name), so Bloom copies and checks it like the rest. --settings also
names the first file as the settings schema Bloom draws the add-on's page from.
"""
import hashlib
import json
import os
import sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    args = sys.argv[1:]
    settings = args[:1] == ["--settings"]
    if settings:
        args = args[1:]
    folder, names = args[0], args[1:]
    path = os.path.join(folder, "addon.json")
    with open(path, encoding="utf-8") as f:
        manifest = json.load(f)
    for name in names:
        if os.path.basename(name) != name:
            raise SystemExit(f"{name}: a file name in the bundle folder, not a path")
        manifest["files"][name] = sha256(os.path.join(folder, name))
    if settings:
        manifest["settings"] = names[0]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"addon.json: {len(manifest['files'])} files")


if __name__ == "__main__":
    main()
