#!/bin/sh
# Wrapper for bindgen that injects a shim header to satisfy missing C11
# atomic types when bindgen's clang processes cross-compiled headers.
#
# We rebuild the argv via `set --` (preserving quoting) and inject:
#   -include <shim>          right after the "--" separator
# so it lands in clang's args, not bindgen's.

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

# Keep the shim in the source tree.  Besides avoiding a write to the host's
# /tmp, this makes the wrapper work when Meson launches MSYS2 bash with a
# Windows PATH (where ordinary Unix utilities may not be discoverable).
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

# Rebuild positional args, inserting clang-side flags right after "--".
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

# Belt-and-suspenders: also export the env var.
export BINDGEN_EXTRA_CLANG_ARGS="--target=aarch64-none-elf -include $SHIM -isystem $CLANG_RES_DIR -isystem $SWITCH_SYS_INCLUDE -isystem $SWITCH_LIBNX_INCLUDE"

exec "$BINDGEN_BIN" "$@"
