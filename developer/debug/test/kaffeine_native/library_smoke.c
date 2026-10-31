/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Native client of the actual library, including deliberate JIT refusal.
    The tiny original 68k HUNKs are written in big-endian order at runtime.
*/
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kaffeine.h>

struct Library *KaffeineBase;
static unsigned checks, failures;
static const char path[] = "RAM:kaffeine-jit-library-smoke.hunk";

static void check(BOOL ok, const char *what)
{
    ++checks;
    if (!ok) ++failures;
    Printf((CONST_STRPTR)"%s %s\n", ok ? "ok  " : "FAIL", what);
}

static void put32(UBYTE **p, ULONG value)
{
    *(*p)++ = value >> 24; *(*p)++ = value >> 16;
    *(*p)++ = value >> 8; *(*p)++ = value;
}

static BOOL hunk(const UWORD *code, ULONG words)
{
    UBYTE bytes[128], *p = bytes;
    ULONG i, longs = (words + 1) / 2;
    const ULONG header[] = {0x3F3, 0, 1, 0, 0, longs, 0x3E9, longs};
    BPTR file;
    LONG written;

    if (words > 32) return FALSE;
    for (i = 0; i < sizeof(header) / sizeof(header[0]); ++i) put32(&p, header[i]);
    for (i = 0; i < words; ++i)
    {
        *p++ = code[i] >> 8;
        *p++ = code[i];
    }
    if (words & 1) { *p++ = 0; *p++ = 0; }
    put32(&p, 0x3F2);
    file = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!file) return FALSE;
    written = Write(file, bytes, p - bytes);
    Close(file);
    return written == p - bytes;
}

static LONG run_hunk(void)
{
    BPTR seg = LoadSeg((CONST_STRPTR)path);
    LONG result;

    if (!seg) return -1;
    result = RunHunk(seg, 0, NULL, 0);
    UnLoadSeg(seg);
    return result;
}

int main(void)
{
    const UWORD simple[] = {0x702A, 0x4E75}; /* MOVEQ #42,D0; RTS */
    const UWORD sr[] = {0x46FC, 0x2700, 0x7000, 0x4E75}; /* MOVE #$2700,SR; return 0 */

    KaffeineBase = OpenLibrary((CONST_STRPTR)"kaffeine.library", 0);
    check(KaffeineBase != NULL, "open actual kaffeine.library");
    if (!KaffeineBase) return 20;
    check(SetVar((CONST_STRPTR)"KAFFEINE_CPU", (CONST_STRPTR)"jit", 3, GVF_LOCAL_ONLY),
          "select JIT in the native caller");
    check(hunk(simple, 2), "write original 68k return program");
    check(RunFile((CONST_STRPTR)path, 0, NULL, 0) == 42, "JIT RunFile returns guest D0");
    check(run_hunk() == 42, "JIT RunHunk accepts a native LoadSeg seglist");
    check(hunk(sr, 4), "write original 68k full-SR program");
    check(RunFile((CONST_STRPTR)path, 0, NULL, 0) == 20, "JIT RunFile refuses full SR before execution");
    check(run_hunk() == 20, "JIT RunHunk refuses full SR before execution");
    check(SetVar((CONST_STRPTR)"KAFFEINE_CPU", (CONST_STRPTR)"moira", 5, GVF_LOCAL_ONLY),
          "select Moira for the next independent run");
    check(RunFile((CONST_STRPTR)path, 0, NULL, 0) == 0, "Moira executes that same valid 68k program");
    check(SetVar((CONST_STRPTR)"KAFFEINE_CPU", (CONST_STRPTR)"invalid", 7, GVF_LOCAL_ONLY),
          "set unknown backend name");
    check(RunFile((CONST_STRPTR)path, 0, NULL, 0) == 20, "unknown backend refuses execution");
    DeleteVar((CONST_STRPTR)"KAFFEINE_CPU", GVF_LOCAL_ONLY);
    DeleteFile((CONST_STRPTR)path);
    CloseLibrary(KaffeineBase);
    Printf((CONST_STRPTR)"library_smoke: %ld checks, %ld failed\n", (LONG)checks, (LONG)failures);
    return failures ? 10 : 0;
}
