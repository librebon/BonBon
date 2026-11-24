#!/bin/sh
# Build dist/<Brand>-<version>-x86_64.AppImage and dist/<brand>-<version>-Linux.tar.gz
# Usage: packaging/build-appimage.sh [build-dir]
# Brand is read from the build directory's CMakeCache (LW_BRAND) or defaults to Libreworks.
set -eu
BUILD=${1:-build}
TOOLS=${TOOLS:-/tmp/linuxdeploy}
mkdir -p "$TOOLS" dist
for t in linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
         linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage; do
    f="$TOOLS/$(basename "$t")"
    [ -x "$f" ] || { curl -fsSL -o "$f" "https://github.com/$t"; chmod +x "$f"; }
done
rm -rf "$TOOLS/AppDir"
DESTDIR="$TOOLS/AppDir" cmake --install "$BUILD" --prefix /usr --strip
export APPIMAGE_EXTRACT_AND_RUN=1 PATH="$TOOLS:$PATH"
export QMAKE=${QMAKE:-$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake)}
VERSION=$(sed -n 's/^project(Libreworks VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
# Brand from the build tree
BRAND=$(grep -E '^LW_BRAND:' "$BUILD/CMakeCache.txt" 2>/dev/null | sed 's/.*=//' || echo Libreworks)
case "$BRAND" in
  Bonbon|bonbon) PRODUCT=Bonbon; PRODUCT_LC=bonbon; LIBDIR=bonbon; APPIMAGE_PREFIX=Bonbon ;;
  *) PRODUCT=Libreworks; PRODUCT_LC=libreworks; LIBDIR=libreworks; APPIMAGE_PREFIX=Libreworks ;;
esac
PLUGINS=$("$QMAKE" -query QT_INSTALL_PLUGINS)
PLATFORMS="libqoffscreen.so"
EXTRA_ARGS=""
if [ -e "$PLUGINS/platforms/libqwayland-generic.so" ]; then
    PLATFORMS="$PLATFORMS;libqwayland-generic.so;libqwayland-egl.so"
    export EXTRA_QT_PLUGINS="waylandclient;wayland-shell-integration;wayland-decoration-client;wayland-graphics-integration-client"
    GI="$TOOLS/AppDir/usr/plugins/wayland-graphics-integration-client"
    mkdir -p "$GI"
    cp "$PLUGINS"/wayland-graphics-integration-client/*.so "$GI"/
    EXTRA_ARGS="--deploy-deps-only=$GI"
else
    echo "warning: qt6-wayland not installed; AppImage will be X11/XWayland only" >&2
fi
HEIF_PLUGINS=$(pkg-config --variable=libdir libheif 2>/dev/null || echo /usr/lib/x86_64-linux-gnu)/libheif/plugins
if [ -d "$HEIF_PLUGINS" ]; then
    mkdir -p "$TOOLS/AppDir/usr/lib/libheif/plugins"
    cp "$HEIF_PLUGINS"/*.so "$TOOLS/AppDir/usr/lib/libheif/plugins/"
    EXTRA_ARGS="$EXTRA_ARGS --deploy-deps-only=$TOOLS/AppDir/usr/lib/libheif/plugins"
fi
[ -d "$TOOLS/AppDir/usr/lib/$LIBDIR" ] && EXTRA_ARGS="$EXTRA_ARGS --deploy-deps-only=$TOOLS/AppDir/usr/lib/$LIBDIR"
export EXTRA_PLATFORM_PLUGINS="$PLATFORMS"
export LDAI_OUTPUT="dist/$APPIMAGE_PREFIX-$VERSION-x86_64.AppImage"
"$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$TOOLS/AppDir" --plugin qt $EXTRA_ARGS --output appimage
for f in "$TOOLS"/AppDir/usr/lib/libheif/plugins/*.so; do [ -e "$f" ] && patchelf --set-rpath '$ORIGIN/../..' "$f"; done
for f in "$TOOLS"/AppDir/usr/lib/"$LIBDIR"/*.so; do [ -e "$f" ] && patchelf --set-rpath '$ORIGIN/..' "$f"; done
"$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$TOOLS/AppDir" --output appimage

TARDIR="$TOOLS/tarball/$PRODUCT_LC-$VERSION-Linux"
rm -rf "$TOOLS/tarball" && mkdir -p "$TOOLS/tarball"
cp -a "$TOOLS/AppDir" "$TARDIR"
tar -C "$TOOLS/tarball" --owner=0 --group=0 -czf "dist/$PRODUCT_LC-$VERSION-Linux.tar.gz" "$PRODUCT_LC-$VERSION-Linux"
echo "wrote dist/$PRODUCT_LC-$VERSION-Linux.tar.gz"
echo "wrote dist/$APPIMAGE_PREFIX-$VERSION-x86_64.AppImage"
