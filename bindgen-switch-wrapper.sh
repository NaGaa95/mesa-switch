#!/bin/sh
# Inject a C11 atomic-type shim into bindgen's Clang arguments after "--".

# Prefer the Docker-installed bindgen, then fall back to the native MSYS2
# CLANGARM64 package used by build-switch-msys2.
if [ -n "${MESA_SWITCH_BINDGEN:-}" ]; then
    BINDGEN_BIN="$MESA_SWITCH_BINDGEN"
elif [ -x /root/.cargo/bin/bindgen ]; then
    BINDGEN_BIN=/root/.cargo/bin/bindgen
elif [ -x /clangarm64/bin/bindgen.exe ]; then
    BINDGEN_BIN=/clangarm64/bin/bindgen.exe
else
    BINDGEN_BIN="$(command -v bindgen)"
fi

# Pass through --version without extra args
case "$*" in
    *--version*) exec "$BINDGEN_BIN" "$@" ;;
esac

# Resolve the in-tree shim with shell builtins for Windows PATH environments.
SCRIPT_DIR=${0%/*}
if [ "$SCRIPT_DIR" = "$0" ]; then
    SCRIPT_DIR=.
fi
SHIM=$SCRIPT_DIR/bindgen-atomic-shim.h

if [ -x /clangarm64/bin/clang.exe ]; then
    CLANG_BIN=/clangarm64/bin/clang.exe
    SWITCH_SYS_INCLUDE=C:/msys64/opt/devkitpro/devkitA64/aarch64-none-elf/include
    SWITCH_LIBNX_INCLUDE=C:/msys64/opt/devkitpro/libnx/include
else
    CLANG_BIN="$(command -v clang)"
    SWITCH_SYS_INCLUDE=${DEVKITPRO:-/opt/devkitpro}/devkitA64/aarch64-none-elf/include
    SWITCH_LIBNX_INCLUDE=${DEVKITPRO:-/opt/devkitpro}/libnx/include
fi
CLANG_RES_DIR="$("$CLANG_BIN" -print-resource-dir)/include"

# Append a sentinel, then rotate args from front to back, injecting after "--".
inject_done=0
set -- "$@" "__BINDGEN_WRAPPER_END__"
out_count=0
while [ "$1" != "__BINDGEN_WRAPPER_END__" ]; do
    arg="$1"
    shift
    set -- "$@" "$arg"
    if [ "$arg" = "--" ] && [ "$inject_done" = "0" ]; then
        set -- "$@" "--target=aarch64-none-elf" \
            "-include" "$SHIM" \
            "-isystem" "$CLANG_RES_DIR" \
            "-isystem" "$SWITCH_SYS_INCLUDE" \
            "-isystem" "$SWITCH_LIBNX_INCLUDE"
        inject_done=1
    fi
done
shift  # drop the sentinel

# Also expose the Clang flags through bindgen's environment variable.
export BINDGEN_EXTRA_CLANG_ARGS="--target=aarch64-none-elf -include $SHIM -isystem $CLANG_RES_DIR -isystem $SWITCH_SYS_INCLUDE -isystem $SWITCH_LIBNX_INCLUDE"

exec "$BINDGEN_BIN" "$@"
