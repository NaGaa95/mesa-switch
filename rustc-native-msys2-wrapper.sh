#!/bin/bash
# Build proc macros for the x64 Windows Rust compiler that loads them.

set -e

SCRIPT_DIR=${0%/*}
if [ "$SCRIPT_DIR" = "$0" ]; then
    SCRIPT_DIR=.
fi

export RUSTUP_HOME="$SCRIPT_DIR/build/deps/rustup"
export CARGO_HOME="$SCRIPT_DIR/build/deps/cargo"
export PATH="/ucrt64/bin:/clang64/bin:$CARGO_HOME/bin:$PATH"
RUSTC_BIN="$CARGO_HOME/bin/rustc.exe"

# Meson records the ARM64 build compiler's linker in its native command.
# Replace that with the x64 CLANG64 linker for Windows proc-macro DLLs.
ARGS=()
skip_next=0
for arg in "$@"; do
    if [ "$skip_next" -eq 1 ]; then
        skip_next=0
        case "$arg" in
            linker=*) continue ;;
        esac
        ARGS+=("-C" "$arg")
        continue
    fi
    case "$arg" in
        -Clinker=*) continue ;;
        -C) skip_next=1 ;;
        *) ARGS+=("$arg") ;;
    esac
done

exec "$RUSTC_BIN" -C linker=C:/msys64/ucrt64/bin/gcc.exe "${ARGS[@]}"
