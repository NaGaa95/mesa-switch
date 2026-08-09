#!/bin/sh
# Run an x64 Mesa host tool from an ARM64 MSYS2 build.  Meson's serialized
# custom-command environment does not retain the CLANG64 DLL directory.

SCRIPT_DIR=${0%/*}
if [ "$SCRIPT_DIR" = "$0" ]; then
    SCRIPT_DIR=.
fi

TOOL=$1
shift
export PATH="/clang64/bin:$PATH"
exec "$SCRIPT_DIR/$TOOL" "$@"
