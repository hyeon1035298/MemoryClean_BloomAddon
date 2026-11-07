#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Builds the Memory Clean add-on into build/addon with PillTaskbar's add-on tools and
# this folder's addon.conf, then adds the admin helper (helper/) to the bundle.
# Targets as in PillTaskbar's tools/build-addon.sh (default all), plus "helper".
# PILL_ROOT is a PillTaskbar_BloomAddon checkout; without it a sibling folder named
# PillTaskbar_BloomAddon or PillTaskbar is used.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
if [[ -z "${PILL_ROOT:-}" ]]; then
    for d in "$here/../PillTaskbar_BloomAddon" "$here/../PillTaskbar"; do
        if [[ -f "$d/tools/build-addon.sh" ]]; then PILL_ROOT="$d"; break; fi
    done
fi
if [[ -z "${PILL_ROOT:-}" || ! -f "$PILL_ROOT/tools/build-addon.sh" ]]; then
    echo "PillTaskbar_BloomAddon not found: set PILL_ROOT to a checkout of it" >&2
    exit 1
fi
PILL_ROOT="$(cd "$PILL_ROOT" && pwd)"
out="${OUT_DIR:-$here/build/addon}"
cxx="${CXX:-/c/Program Files/Windhawk/Compiler/bin/clang++.exe}"
echo "tools: $PILL_ROOT"

build_helper() {
    mkdir -p "$out"
    "$cxx" -target x86_64-w64-mingw32 -Os -std=c++23 -municode -mwindows -static -s \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
        "$here/helper/helper.cpp" -o "$out/MemoryCleanHelper.exe" \
        -lole32 -loleaut32 -luuid -ltaskschd -ladvapi32 -lpsapi -lshell32 -luser32 -lntdll
    echo "built $out/MemoryCleanHelper.exe"
}

targets=("$@")
[[ ${#targets[@]} -eq 0 ]] && targets=(all)
pill=()
for t in "${targets[@]}"; do
    case "$t" in
        helper) ;;
        *) pill+=("$t") ;;
    esac
done
if [[ ${#pill[@]} -gt 0 ]]; then
    ADDON_CONF="$here/addon.conf" OUT_DIR="$out" bash "$PILL_ROOT/tools/build-addon.sh" "${pill[@]}"
fi
for t in "${targets[@]}"; do
    if [[ "$t" == all || "$t" == helper ]]; then build_helper; fi
done
# The settings page Bloom draws: this folder's hand-written schema, listed in addon.json.
if [[ -f "$out/addon.json" ]]; then
    cp "$here/settings.schema.json" "$out/settings.schema.json"
    python -I "$here/tools/add-files.py" --settings "$out" settings.schema.json
fi
# addon.json lists only PillTaskbar's files; the helper joins them with its hash.
if [[ -f "$out/addon.json" && -f "$out/MemoryCleanHelper.exe" ]]; then
    python -I "$here/tools/add-files.py" "$out" MemoryCleanHelper.exe
fi
