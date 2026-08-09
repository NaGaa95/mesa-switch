#!/bin/bash
# Select an ELF AArch64 Rust target for Switch code.  Meson otherwise sees the
# ARM64 Windows host compiler and silently emits COFF objects that cannot be
# linked into an NRO.

set -e

SCRIPT_DIR=${0%/*}
if [ "$SCRIPT_DIR" = "$0" ]; then
    SCRIPT_DIR=.
fi

RUST_TARGET=${MESA_SWITCH_RUST_TARGET:-}

if [ -n "${MESA_SWITCH_RUSTC:-}" ]; then
    RUSTC_BIN="$MESA_SWITCH_RUSTC"
elif [ -x /root/.cargo/bin/rustc ]; then
    RUSTC_BIN=/root/.cargo/bin/rustc
elif [ -x "$SCRIPT_DIR/build/deps/cargo/bin/rustc.exe" ]; then
    export RUSTUP_HOME="$SCRIPT_DIR/build/deps/rustup"
    export CARGO_HOME="$SCRIPT_DIR/build/deps/cargo"
    export PATH="/ucrt64/bin:/clang64/bin:$CARGO_HOME/bin:$PATH"
    RUSTC_BIN="$CARGO_HOME/bin/rustc.exe"
    RUST_TARGET=${RUST_TARGET:-aarch64-unknown-linux-gnu}
elif [ -x /clangarm64/bin/rustc.exe ]; then
    RUSTC_BIN=/clangarm64/bin/rustc.exe
else
    RUSTC_BIN="$(command -v rustc)"
fi

# Meson's compiler probe only needs a runnable host binary.  Real Mesa Rust
# targets retain the devkitA64 linker and use the ELF target selected above.
for arg in "$@"; do
    case "$arg" in
        *sanity_check_for_rust.rs*|*sanitycheckrs.rs*) RUST_TARGET= ;;
    esac
done

if [ -n "$RUST_TARGET" ]; then
    exec "$RUSTC_BIN" --target="$RUST_TARGET" "$@"
fi

# Native sanity fallback: remove the cross linker from Meson's compiler
# command so the Windows-host compiler can perform the probe.
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
