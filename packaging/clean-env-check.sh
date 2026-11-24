#!/bin/sh
# Run the self-contained tarball and the AppImage in a bubblewrap sandbox.
# Usage: packaging/clean-env-check.sh [out-dir] [build-dir]
# Brand is read from build-dir's CMakeCache (default: build).
set -eu
VERSION=$(sed -n 's/^project(Libreworks VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
OUT=$(realpath -m "${1:-dist/clean-env}")
BUILD=${2:-build}
BRAND=$(grep -E '^LW_BRAND:' "$BUILD/CMakeCache.txt" 2>/dev/null | sed 's/.*=//' || echo Libreworks)
case "$BRAND" in
  Bonbon|bonbon) PRODUCT=Bonbon; PRODUCT_LC=bonbon; ICON=packaging/bonbon.png ;;
  *) PRODUCT=Libreworks; PRODUCT_LC=libreworks; ICON=packaging/libreworks.png ;;
esac
WORK=$(mktemp -d)
mkdir -p "$OUT" "$WORK/tb"
tar xzf "dist/$PRODUCT_LC-$VERSION-Linux.tar.gz" -C "$WORK/tb"
LIB=/usr/lib/x86_64-linux-gnu
HIDE=""
for f in $LIB/libQt6* $LIB/libtiff* $LIB/libwebp* $LIB/libavif* $LIB/libjxl* $LIB/libgif* $LIB/libheif*; do
    if [ -f "$f" ] && [ ! -L "$f" ]; then HIDE="$HIDE --ro-bind /dev/null $f"; fi
done
QTDIR=""
[ -d $LIB/qt6 ] && QTDIR="--tmpfs $LIB/qt6"
icon=$(realpath "$ICON")
# shellcheck disable=SC2086
bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin \
    --ro-bind /etc/fonts /etc/fonts --proc /proc --dev /dev --tmpfs /tmp --tmpfs /home/u $HIDE $QTDIR \
    --ro-bind "$WORK/tb" /opt/tb --ro-bind "dist/$PRODUCT-$VERSION-x86_64.AppImage" /opt/App.AppImage \
    --ro-bind "$icon" /opt/in.png --bind "$OUT" /out \
    --unshare-all --die-with-parent --clearenv --setenv HOME /home/u --setenv PATH /usr/bin:/bin \
    --setenv LANG C.UTF-8 --setenv QT_QPA_PLATFORM offscreen --setenv APPIMAGE_EXTRACT_AND_RUN 1 \
    /bin/sh -ec "
        B=/opt/tb/$PRODUCT_LC-$VERSION-Linux/usr/bin/$PRODUCT_LC
        \$B --version
        \$B --sample --select Star --screenshot /out/clean-env-tarball.png
        cd /tmp
        /opt/App.AppImage --version
        /opt/App.AppImage --sample --select Star --screenshot /out/clean-env-appimage.png
        printf '{\"steps\":[{\"type\":\"scale\",\"mode\":\"percent\",\"percent\":50},{\"type\":\"export\"}],\"output\":\"folder\"}' > /tmp/s.json
        \$B --batch /tmp/s.json --out /out/batch /opt/in.png
        /opt/App.AppImage --batch /tmp/s.json --out /out/batch-appimage --quiet /opt/in.png
        ls /out/batch /out/batch-appimage
        echo \"clean-env check OK ($PRODUCT)\""
rm -rf "$WORK"
