/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef DOMAIN_FIXTURE_HUNKS_H
#define DOMAIN_FIXTURE_HUNKS_H
#include <stddef.h>
#include <stdint.h>

#define DF_ROOT_ARGUMENTS "argcopy-ok\n"
#define DF_CHILD_ARGUMENTS "child-private-args\n"
#define DF_MESSAGE_SIZE 128
#define DF_ROOT_IOERR UINT32_C(0x12340555)
#define DF_CHILD_IOERR UINT32_C(0x21212121)
#define DF_ROOT_FLAGS UINT32_C(7)
#define DF_CHILD_FLAGS UINT32_C(0x1ff)

/* Original literal 68000 code and its data, emitted as one executable HUNK
   with explicit RELOC32 metadata. No native or foreign executable is read.
   Functions are serial/non-reentrant; zero reports an emitter bound error. */
size_t df_build_root(uint8_t *, size_t, const char *child_process_name);
size_t df_build_child(uint8_t *, size_t, const char *backend,
                      const char *absolute_failure_proof);
#endif
