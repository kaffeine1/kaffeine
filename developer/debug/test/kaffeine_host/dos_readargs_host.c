/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Host build of the native argument parser for the kaffeine ReadArgs
    test: rom/dos/readargs.c and the ReadItem(), FindArg(), StrToLong() and
    FreeArgs() it uses, compiled unchanged. The library function macros
    become plain C functions; exec, the streams and the error code come
    from the test through readargs_stubs.
*/
#include <aros/libcall.h>
#include <aros/debug.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <dos/rdargs.h>
#include <dos/dosextens.h>

/* Keep the real DOS internal header out of the host build. */
#define DOS_INTERN_H

/* From rom/dos/dos_intern.h */
#define RDAF_ALLOCATED_BY_READARGS (1L << 31)
struct DAList
{
    STRPTR *ArgBuf;
    UBYTE  *StrBuf;
    STRPTR *MultVec;
    BOOL    FreeRDA;
};
#ifndef EOF
#define EOF -1
#endif

#define ASSERT_VALID_PTR(p) ((void)(p))
#define ASSERT_VALID_PTR_OR_NULL(p) ((void)(p))
#define ASSERT_VALID_PROCESS(p) ((void)(p))

#undef AROS_LHA
#undef AROS_LIBFUNC_INIT
#undef AROS_LIBFUNC_EXIT
#define AROS_LHA(type, name, reg) type name
#define AROS_LH2(type, fn, a1, a2, bt, bn, o, lib) type fn(a1, a2)
#define AROS_LH2I AROS_LH2
#define AROS_LH3(type, fn, a1, a2, a3, bt, bn, o, lib) type fn(a1, a2, a3)
#define AROS_NTLH1(type, fn, a1, bt, bn, o, lib) type fn(a1)
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT

#include "../../../../rom/dos/strtolong.c"
#include "../../../../rom/dos/findarg.c"
#include "../../../../rom/dos/readitem.c"
#include "../../../../rom/dos/freeargs.c"
#include "../../../../rom/dos/readargs.c"
