#ifndef BINDGEN_ATOMIC_SHIM_H
#define BINDGEN_ATOMIC_SHIM_H

/* Skip stdatomic.h entirely: bindgen only needs these types for layout. */
#define _STDATOMIC_H 1
#define _STDATOMIC_H_ 1
#define __CLANG_STDATOMIC_H 1
#define __STDATOMIC_H 1

#include <stdint.h>
#include <stddef.h>

#define _Atomic(T) T

typedef _Bool             atomic_bool;
typedef char              atomic_char;
typedef signed char       atomic_schar;
typedef unsigned char     atomic_uchar;
typedef short             atomic_short;
typedef unsigned short    atomic_ushort;
typedef int               atomic_int;
typedef unsigned int      atomic_uint;
typedef long              atomic_long;
typedef unsigned long     atomic_ulong;
typedef long long         atomic_llong;
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
