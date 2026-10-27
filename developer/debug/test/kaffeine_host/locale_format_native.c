/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Compile the unchanged AROS renderer as an independent host oracle.
    Tests supply aligned native arguments and explicit positional offsets.
    Only InternalFormatString is used, not the native index allocator.
*/
#include <stdint.h>
#include <stdlib.h>
#include <exec/types.h>
#include <proto/locale.h>

typedef int64_t QUAD;
typedef uint64_t UQUAD;
typedef uintptr_t BSTR;
typedef const void *CONST_APTR;
typedef void *RAWARG;
#define VOID void
#define LOCALE_INTERN_H
#define USE_GLOBALLIMIT 1
#define BNULL 0
#define AROS_BSTR_ADDR(s) ((UBYTE *)(uintptr_t)(s) + 1)
#define AROS_BSTR_strlen(s) (*(UBYTE *)(uintptr_t)(s))
#define AROS_LHA(type, name, reg) type name
#define AROS_LH4(type, fn, a, b, c, d, bt, bn, o, lib) type fn(a, b, c, d)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#ifndef alloca
#define alloca __builtin_alloca
#endif
#define FormatString locale_native_FormatString
#define InternalFormatString locale_native_InternalFormatString

static void localeDataStreamFromFormat(CONST_STRPTR format, APTR stream,
    ULONG *size, ULONG *indices, ULONG *index_size)
{
    (void)format; (void)stream; (void)size; (void)indices; (void)index_size;
    abort();
}

/* The imported C uses UBYTE * strings, signed loop indices and an intentional
   pointer/hex fallthrough. Keep adapter and fixture diagnostics enabled. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-sign"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "../../../../workbench/libs/locale/formatstring.c"
#pragma GCC diagnostic pop
