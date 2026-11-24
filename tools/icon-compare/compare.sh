#!/bin/sh
# Build <commit-a> and <commit-b> in <out>/ and compare their panel icons (prints differing pixels).
set -eu
A=$1; B=$2; OUT=$3; HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(git -C "$HERE" rev-parse --show-toplevel)
LIBS="$(pkg-config --libs Qt6Widgets Qt6Gui Qt6Concurrent libwebp libwebpdemux libwebpmux libavif libjxl libjxl_threads zlib libtiff-4) -lgif"
export QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME="$OUT/cfg"
for c in "$A" "$B"; do
    d="$OUT/$c"; rm -rf "$d"; mkdir -p "$d"
    git -C "$REPO" archive "$c" | tar -x -C "$d"
    cmake -S "$d" -B "$d/b" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && ninja -C "$d/b" lwui lwcore >/dev/null
    g++ -std=c++17 -fPIC -O1 "$HERE/panel_icons.cpp" -I"$d/src" -o "$d/panel_icons" "$d/b/liblwui.a" "$d/b/liblwcore.a" $(pkg-config --cflags Qt6Widgets) $LIBS
    ENABLED=1 "$d/panel_icons" "$OUT/$c-enabled.png" >/dev/null 2>&1
    "$d/panel_icons" "$OUT/$c-disabled.png" >/dev/null 2>&1
done
for s in enabled disabled; do
    printf '%s: ' "$s"; compare -metric AE "$OUT/$A-$s.png" "$OUT/$B-$s.png" null: 2>&1 || true; echo
done
