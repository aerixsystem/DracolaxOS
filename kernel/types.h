/* kernel/types.h
 * Self-contained freestanding type definitions for x86_64.
 * Replaces <stdint.h>, <stddef.h>, <stdarg.h>, <stdbool.h>.
 * No host headers required — works with -nostdinc.
 */
#ifndef TYPES_H
#define TYPES_H

/* ---- stdint.h ------------------------------------------------------------ */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

/* x86_64: pointers are 64-bit */
typedef uint64_t            uintptr_t;
typedef int64_t             intptr_t;

#define UINT8_MAX   0xFF
#define UINT16_MAX  0xFFFF
#define UINT32_MAX  0xFFFFFFFFU
#define UINT64_MAX  0xFFFFFFFFFFFFFFFFULL
#define INT32_MIN   (-2147483648)
#define INT32_MAX   2147483647
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   9223372036854775807LL

/* ---- stddef.h ------------------------------------------------------------ */
#define NULL            ((void *)0)
/* x86_64: sizeof(void*) = 8, sizeof(long) = 8 */
typedef uint64_t        size_t;
typedef int64_t         ssize_t;
typedef int64_t         ptrdiff_t;
#define offsetof(type, member) ((size_t)&(((type *)0)->member))

/* ---- stdbool.h ----------------------------------------------------------- */
typedef _Bool           bool;
#define true            1
#define false           0

/* ---- stdarg.h ------------------------------------------------------------ */
typedef __builtin_va_list va_list;
#define va_start(v, l)  __builtin_va_start(v, l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v, l)    __builtin_va_arg(v, l)
#define va_copy(d, s)   __builtin_va_copy(d, s)

#endif /* TYPES_H */
