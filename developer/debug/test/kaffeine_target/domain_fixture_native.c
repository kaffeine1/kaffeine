/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Native AROS/ARM controller for an original shared-arena 68k regression.
   Run under an external VM deadline as RunHunk itself is the operation
   under test; all controller/child polls have explicit finite limits. */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>
#include <proto/kaffeine.h>
#include <stdio.h>
#include <string.h>
#include "domain_fixture_hunks.h"

#if defined(__AROSEXEC_SMP__)
#error This retirement-identity regression requires uniprocessor Exec
#endif

APTR KernelBase;
struct Library *KaffeineBase;
static unsigned checks, failures;
static UBYTE image[32768], proof[DF_MESSAGE_SIZE + 1], old_backend[4096];
static ULONG old_backend_length, old_backend_flags;
static BOOL old_backend_present;

static BOOL check(BOOL good, const char *what)
{
    ++checks;
    if (!good) ++failures;
    printf("%s %s\n", good ? "ok  " : "FAIL", what);
    fflush(stdout);
    return good;
}

static uint32_t be32(const UBYTE *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static BOOL write_file(const char *path, const void *bytes, LONG length)
{
    BPTR f = Open(path, MODE_NEWFILE);
    LONG n;
    BOOL closed;
    if (!f) return FALSE;
    n = Write(f, bytes, length);
    closed = Close(f);
    return n == length && closed;
}

static LONG read_file(const char *path, void *bytes, LONG capacity)
{
    BPTR f = Open(path, MODE_OLDFILE);
    LONG n;
    if (!f) return -1;
    n = Read(f, bytes, capacity);
    return Close(f) ? n : -1;
}

static BOOL drawer(const char *path)
{
    BPTR lock = CreateDir(path);
    if (!lock) lock = Lock(path, ACCESS_READ);
    if (!lock) return FALSE;
    UnLock(lock);
    return TRUE;
}

static BOOL absent(const char *path)
{
    BPTR lock = Lock(path, ACCESS_READ);
    if (lock) { UnLock(lock); return FALSE; }
    return IoErr() == ERROR_OBJECT_NOT_FOUND;
}

static BOOL clean_leaf(const char *path)
{
    if (!DeleteFile(path) && IoErr() != ERROR_OBJECT_NOT_FOUND) return FALSE;
    return absent(path);
}

static BOOL save_local_backend(void)
{
    const struct LocalVar *var = FindVar("KAFFEINE_CPU", LV_VAR);
    old_backend_present = var != NULL;
    old_backend_length = old_backend_flags = 0;
    if (!var) return TRUE;
    if (var->lv_Len > sizeof(old_backend)) return FALSE;
    old_backend_length = var->lv_Len;
    old_backend_flags = var->lv_Flags;
    if (var->lv_Len) memcpy(old_backend, var->lv_Value, var->lv_Len);
    return TRUE;
}

static BOOL restore_local_backend(void)
{
    const struct LocalVar *var;
    if (old_backend_present)
    {
        if (!SetVar("KAFFEINE_CPU", (CONST_STRPTR)old_backend, old_backend_length,
                    GVF_LOCAL_ONLY | old_backend_flags)) return FALSE;
    }
    else if (!DeleteVar("KAFFEINE_CPU", GVF_LOCAL_ONLY) &&
             FindVar("KAFFEINE_CPU", LV_VAR)) return FALSE;
    var = FindVar("KAFFEINE_CPU", LV_VAR);
    return (!!var == !!old_backend_present) &&
        (!var || (var->lv_Len == old_backend_length && var->lv_Flags == old_backend_flags &&
                  !memcmp(var->lv_Value, old_backend, old_backend_length)));
}

int main(int argc, char **argv)
{
    const char *backend = argc > 1 ? argv[1] : "moira";
    const char *opposite;
    char root[96], child[112], path[160], proof_path[160], gate[160], process_name[80];
    BPTR segment = BNULL, run_directory = BNULL, prior_directory = BNULL;
    BPTR saved_input = Input(), saved_output = Output();
    char *native_args = NULL, *reuse = NULL;
    size_t bytes;
    unsigned tick;
    BOOL backend_changed = FALSE, directory_changed = FALSE;
    BOOL got_proof = FALSE, caller_ioerr_private = TRUE, retired = FALSE;
    LONG root_result = -1;

    if (!check(argc <= 2 && (!strcmp(backend, "moira") || !strcmp(backend, "jit")),
               "select one explicit guest backend")) goto done;
    opposite = !strcmp(backend, "moira") ? "jit" : "moira";
    KernelBase = OpenResource("kernel.resource");
    if (!check(KernelBase && KrnGetCPUCount() == 1,
               "require exactly one active CPU for bounded name-based retirement")) goto done;
    if (!check(Cli() != NULL, "run the native controller from a CLI")) goto done;
    snprintf(root, sizeof(root), "RAM:kaffeine-domain-%s", backend);
    snprintf(child, sizeof(child), "%s/child", root);
    snprintf(process_name, sizeof(process_name), "kaffeine-domain-child-%s", backend);
    snprintf(proof_path, sizeof(proof_path), "%s/proof", child);
    snprintf(gate, sizeof(gate), "%s/release", child);
    if (!check(!FindTask("kaffeine") && !FindTask(process_name),
               "no fixture driver or child broker is already running")) goto done;
    KaffeineBase = OpenLibrary("kaffeine.library", 0);
    if (!check(KaffeineBase != NULL, "pin the public library through domain retirement")) goto done;
    if (!check(save_local_backend(), "snapshot only this controller's exact local backend variable")) goto done;
    if (!check(SetVar("KAFFEINE_CPU", backend, strlen(backend), GVF_LOCAL_ONLY),
               "select the backend in this native caller's local variables")) goto done;
    backend_changed = TRUE;
    if (!check(drawer(root) && drawer(child), "prepare the reserved RAM fixture directories")) goto done;
    if (!check(clean_leaf(proof_path) && clean_leaf(gate), "remove only prior fixture gate and proof")) goto done;
    snprintf(path, sizeof(path), "%s/proof", root);
    if (!check(clean_leaf(path), "exclude a stale proof in the parent directory")) goto done;
    snprintf(path, sizeof(path), "%s/root-input", root);
    if (!check(write_file(path, "ROOTDIR", 7), "write the root's relative directory sentinel")) goto done;
    snprintf(path, sizeof(path), "%s/child-input", child);
    if (!check(write_file(path, "CHILDDIR", 8), "write the child's distinct directory sentinel")) goto done;

    bytes = df_build_child(image, sizeof(image), backend, proof_path);
    snprintf(path, sizeof(path), "%s/domain_child", root);
    if (!check(bytes && bytes <= 32768 && write_file(path, image, (LONG)bytes),
               "emit the original relocated 68k child without a 68k compiler")) goto done;
    bytes = df_build_root(image, sizeof(image), process_name);
    snprintf(path, sizeof(path), "%s/domain_root", root);
    if (!check(bytes && bytes <= 32768 && write_file(path, image, (LONG)bytes),
               "emit the original root with typed CNP tags and opaque 128-byte message")) goto done;
    native_args = AllocVec(128, MEMF_PUBLIC | MEMF_CLEAR);
    if (!check(native_args != NULL, "allocate caller-owned native argument storage")) goto done;
    strcpy(native_args, DF_ROOT_ARGUMENTS);
    run_directory = Lock(root, ACCESS_READ);
    if (!check(run_directory != BNULL, "acquire the native root current directory")) goto done;
    prior_directory = CurrentDir(run_directory);
    run_directory = BNULL;
    directory_changed = TRUE;
    segment = LoadSeg("domain_root");
    if (!check(segment != BNULL, "native DOS loads the newly emitted root hunk")) goto done;
    {
        IPTR hunk = 0, relocs = 0;
        struct TagItem tags[] = {{GSLI_68KHUNK, (IPTR)&hunk},
            {GSLI_68KRelocs, (IPTR)&relocs}, {TAG_DONE, 0}};
        if (!check(GetSegListInfo(segment, tags) == 2 && hunk && relocs,
                   "native LoadSeg records the root's 68k identity and relocation metadata")) goto done;
    }

    root_result = RunHunk(segment, 65536, native_args, strlen(native_args));
    check(root_result == 55, "RunHunk with a nonzero stack returns the root's 55 while its child lives");
    memset(native_args, 'X', 128);
    FreeVec(native_args); native_args = NULL;
    reuse = AllocVec(128, MEMF_PUBLIC);
    if (reuse) memset(reuse, 'Z', 128);
    UnLoadSeg(segment); segment = BNULL;
    run_directory = CurrentDir(prior_directory);
    prior_directory = BNULL; directory_changed = FALSE;
    if (run_directory) { UnLock(run_directory); run_directory = BNULL; }
    check(SetVar("KAFFEINE_CPU", opposite, strlen(opposite), GVF_LOCAL_ONLY),
          "mutate the native caller's backend after RunHunk returns");
    check(Input() == saved_input && Output() == saved_output,
          "root completion preserves the native caller's selected streams");
    if (root_result == 55)
    {
        check(FindTask("kaffeine") && FindTask(process_name),
              "the driver and private broker are still live after native root return");
        check(absent(proof_path), "the child has not published proof before the caller release gate");
    }
    check(write_file(gate, "GATE1\n", 6), "release the child after arguments, source seglist and caller directory change");
    for (tick = 0; tick < 1500; ++tick)
    {
        LONG length;
        SetIoErr(0x71717171);
        Delay(1);
        if (IoErr() != 0x71717171) caller_ioerr_private = FALSE;
        length = read_file(proof_path, proof, sizeof(proof));
        if (length == DF_MESSAGE_SIZE) { got_proof = TRUE; break; }
        if (!FindTask("kaffeine") && !FindTask(process_name)) break;
    }
    check(caller_ioerr_private, "child DOS brokers preserve the native caller's IoErr across native yields");
    if (check(got_proof, "the child publishes one complete bounded proof after caller mutation"))
    {
        if (be32(proof) == 0x4b444631)
            printf("child failure code: %lu\n", (unsigned long)be32(proof + 4));
        check(be32(proof + 20) == 0x4b443031 && proof[18] == 0 && proof[19] == 128,
              "proof retains the original 128-byte guest message protocol");
        check(be32(proof + 28) && be32(proof + 28) < 32 * 1024 * 1024 &&
              be32(proof + 28) == be32(proof + 32) && be32(proof + 28) == be32(proof + 36),
              "root and child observed the exact same message address across ReplyMsg and WaitPort");
        check(be32(proof + 24) && be32(proof + 24) < 32 * 1024 * 1024 &&
              be32(proof + 24) == be32(proof + 40) && be32(proof + 24) == be32(proof + 44) &&
              !memcmp(proof + 108, DF_ROOT_ARGUMENTS, sizeof(DF_ROOT_ARGUMENTS)),
              "the nested guest pointer and copied caller arguments survive native source mutation and release");
        check(!memcmp(proof + 88, DF_CHILD_ARGUMENTS, sizeof(DF_CHILD_ARGUMENTS)),
              "typed arguments survive overwriting their original root-module source");
        check(be32(proof + 48) == DF_ROOT_FLAGS && be32(proof + 52) == DF_CHILD_FLAGS &&
              be32(proof + 56) == DF_ROOT_IOERR && be32(proof + 60) == DF_CHILD_IOERR,
              "root and broker DOS preserve independent directories, home, CLI, streams, backend and IoErr");
        check(be32(proof + 64) && be32(proof + 68) && be32(proof + 64) != be32(proof + 68) &&
              be32(proof + 72) && be32(proof + 76) && be32(proof + 72) != be32(proof + 76),
              "the same domain contains distinct guest Processes and CLI objects");
        check(be32(proof + 80) == 0x47415445 && !be32(proof + 120) && be32(proof + 124) == 7,
              "the child reached the post-return gate and requested its own Exit(7)");
    }
    for (tick = 0; tick < 500; ++tick)
    {
        if (!FindTask("kaffeine") && !FindTask(process_name)) { retired = TRUE; break; }
        Delay(1);
    }
    check(retired, "both native owners retire within the bounded deadline after child Exit");
    snprintf(path, sizeof(path), "%s/proof", root);
    check(absent(path), "the child proof used its private current directory");
    check(Input() == saved_input && Output() == saved_output,
          "child retirement preserves the caller's selected native streams");

done:
    if (segment) UnLoadSeg(segment);
    if (directory_changed)
    {
        run_directory = CurrentDir(prior_directory);
        if (run_directory) UnLock(run_directory);
        run_directory = BNULL;
    }
    if (run_directory) UnLock(run_directory);
    if (native_args) FreeVec(native_args);
    if (reuse) FreeVec(reuse);
    if (backend_changed) check(restore_local_backend(), "restore the exact native caller local backend variable");
    if (KaffeineBase) CloseLibrary(KaffeineBase);
    printf("domain_fixture_native: %u checks, %u failed\n", checks, failures);
    fflush(stdout);
    return failures ? 20 : 0;
}
