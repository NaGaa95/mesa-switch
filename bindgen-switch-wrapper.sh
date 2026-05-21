#!/bin/sh
# Wrapper for bindgen that injects a shim header to satisfy missing C11
# atomic types when bindgen's clang processes cross-compiled headers.
#
# We rebuild the argv via `set --` (preserving quoting) and inject:
#   -include <shim>          right after the "--" separator
# so it lands in clang's args, not bindgen's.

# Pass through --version without extra args
case "$*" in
    *--version*) exec /root/.cargo/bin/bindgen "$@" ;;
esac

SHIM_DIR=/tmp/bindgen-shim
SHIM=$SHIM_DIR/atomic_shim.h
mkdir -p "$SHIM_DIR"
cat > "$SHIM" << 'EOF'
#ifndef BINDGEN_ATOMIC_SHIM_H
#define BINDGEN_ATOMIC_SHIM_H

/* Skip stdatomic.h entirely — pre-define its common include guards. */
#define _STDATOMIC_H 1
#define _STDATOMIC_H_ 1
#define __CLANG_STDATOMIC_H 1
#define __STDATOMIC_H 1

#include <stdint.h>
#include <stddef.h>

/* C11 atomic types — for bindgen we just need struct layout. */
#define _Atomic(T) T

typedef _Bool          atomic_bool;
typedef char           atomic_char;
typedef signed char    atomic_schar;
typedef unsigned char  atomic_uchar;
typedef short          atomic_short;
typedef unsigned short atomic_ushort;
typedef int            atomic_int;
typedef unsigned int   atomic_uint;
typedef long           atomic_long;
typedef unsigned long  atomic_ulong;
typedef long long          atomic_llong;
typedef unsigned long long atomic_ullong;

typedef int_least8_t   atomic_int_least8_t;
typedef uint_least8_t  atomic_uint_least8_t;
typedef int_least16_t  atomic_int_least16_t;
typedef uint_least16_t atomic_uint_least16_t;
typedef int_least32_t  atomic_int_least32_t;
typedef uint_least32_t atomic_uint_least32_t;
typedef int_least64_t  atomic_int_least64_t;
typedef uint_least64_t atomic_uint_least64_t;

typedef int_fast8_t   atomic_int_fast8_t;
typedef uint_fast8_t  atomic_uint_fast8_t;
typedef int_fast16_t  atomic_int_fast16_t;
typedef uint_fast16_t atomic_uint_fast16_t;
typedef int_fast32_t  atomic_int_fast32_t;
typedef uint_fast32_t atomic_uint_fast32_t;
typedef int_fast64_t  atomic_int_fast64_t;
typedef uint_fast64_t atomic_uint_fast64_t;

typedef intptr_t  atomic_intptr_t;
typedef uintptr_t atomic_uintptr_t;
typedef size_t    atomic_size_t;
typedef ptrdiff_t atomic_ptrdiff_t;

typedef enum {
    memory_order_relaxed,
    memory_order_consume,
    memory_order_acquire,
    memory_order_release,
    memory_order_acq_rel,
    memory_order_seq_cst
} memory_order;

typedef struct { atomic_bool _val; } atomic_flag;

#endif /* BINDGEN_ATOMIC_SHIM_H */
EOF

CLANG_RES_DIR="$(clang -print-resource-dir 2>/dev/null || echo /usr/lib/llvm-15/lib/clang/15.0.6)/include"

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
        set -- "$@" "-include" "$SHIM" \
            "-isystem" "$CLANG_RES_DIR" \
            "-isystem" "/usr/include" \
            "-isystem" "/usr/include/aarch64-linux-gnu"
        inject_done=1
    fi
done
shift  # drop the sentinel

# Belt-and-suspenders: also export the env var.
export BINDGEN_EXTRA_CLANG_ARGS="-include $SHIM -isystem $CLANG_RES_DIR -isystem /usr/include -isystem /usr/include/aarch64-linux-gnu"

exec /root/.cargo/bin/bindgen "$@"
