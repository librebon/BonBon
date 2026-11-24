#!/bin/sh
# CPackDeb runs dpkg-shlibdeps in the package staging tree. Drop
# packaging/debian/shlibs.local into that tree first so dependency floors
# stay at the Debian trixie base versions, not a security update the build
# box happened to install.
set -e
here=$(dirname "$(readlink -f "$0")")
mkdir -p debian
if [ -f "$here/debian/shlibs.local" ]; then
    cp "$here/debian/shlibs.local" debian/shlibs.local
fi
exec /usr/bin/dpkg-shlibdeps "$@"
