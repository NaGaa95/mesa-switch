#!/bin/bash
# Strip meson's forced -C linker=<cross-gcc> so rustc's sanity check can
# link with the native system linker. NAK only emits static rlibs in the
# actual build, so the linker is never invoked for real output.

set -e

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

exec /root/.cargo/bin/rustc "${ARGS[@]}"
