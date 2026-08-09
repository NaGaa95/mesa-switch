#!/bin/bash
# Make devkitPro's POSIX pkg-config metadata usable by Windows-native Meson.
set -o pipefail

export PKG_CONFIG_DIR=
export PKG_CONFIG_PATH=
export PKG_CONFIG_SYSROOT_DIR=
export PKG_CONFIG_LIBDIR=/opt/devkitpro/portlibs/switch/lib/pkgconfig

/usr/bin/pkg-config --static "$@" |
    /usr/bin/sed -e 's|/opt/devkitpro|C:/msys64/opt/devkitpro|g' \
        -e 's|$(DEVKITPRO)|C:/msys64/opt/devkitpro|g'
