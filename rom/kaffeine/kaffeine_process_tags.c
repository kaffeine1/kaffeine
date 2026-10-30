/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Typed startup for a registered independent 68k program. Code pointers,
   callbacks, stream transfers and borrowed modules are not forwarded. */
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h>
#include "kaffeine_process_tags.h"
#include "kaffeine_process.h"
#include "kaffeine_segments.h"
#include "kaffeine_locks.h"
#include "kaffeine_taglist.h"

static ULONG fail(SIPTR error)
{
    SetIoErr(error);
    return 0;
}

/* Copy before creating any native task. Never retain a guest string across
   startup or allow a scan past the field's documented local capacity. */
static BOOL text_copy(struct KaffeineContext *ctx, ULONG address,
                      char *out, ULONG capacity)
{
    const UBYTE *end;
    ULONG available;
    if (!ctx->mem || !address || address >= ctx->mem_size) return FALSE;
    available = ctx->mem_size - address;
    if (available > capacity) available = capacity;
    end = memchr(ctx->mem + address, 0, available);
    if (!end) return FALSE;
    memcpy(out, ctx->mem + address, end - (ctx->mem + address) + 1);
    return TRUE;
}

ULONG Kaffeine_ProcessFromTags(struct KaffeineContext *ctx, ULONG tags)
{
    struct KaffeineTagWalk walk;
    struct KaffeineProcessStart spec;
    struct KaffeineSegmentTransfer module;
    struct KaffeineLockTransfer home;
    struct Task *task;
    ULONG item, seen = 0, segment = 0, directory = 0;
    ULONG name_ptr = 0, command_ptr = 0, args_ptr = 0, result;
    LONG error = 0, backend_length;
    SIPTR saved_error;
    int next;
    char name[256] = "68k process", command[256], arguments[4096] = "";
    char backend[16] = {0};

    if (!ctx || !ctx->mem) return fail(ERROR_BAD_NUMBER);
    memset(&spec, 0, sizeof(spec));
    spec.stack_size = KAFFEINE_STACK_SIZE;
    spec.copy_vars = TRUE;
    task = FindTask(NULL);
    spec.priority = task ? task->tc_Node.ln_Pri : 0;
    Kaffeine_TagWalkInit(&walk, tags);
    walk.limit = 64;
    while ((next = Kaffeine_TagWalkNext(ctx, &walk, &item)) == KAFFEINE_TAG_ITEM)
    {
        ULONG tag = m68k_read32(ctx, item), data = m68k_read32(ctx, item + 4);
        ULONG bit;
        if (tag <= NP_Dummy || tag > NP_Dummy + 31)
        { if (!error) error = ERROR_NOT_IMPLEMENTED; continue; }
        bit = 1UL << (tag - NP_Dummy - 1);
        if (seen & bit) { error = ERROR_BAD_NUMBER; continue; }
        seen |= bit;
        switch (tag)
        {
        case NP_Seglist: segment = data; break;
        case NP_FreeSeglist:
            if (data != TRUE && !error) error = ERROR_NOT_IMPLEMENTED;
            break;
        case NP_StackSize:
            if (data < 256 || data > KAFFEINE_STACK_SIZE) error = ERROR_BAD_NUMBER;
            else spec.stack_size = data;
            break;
        case NP_Name: name_ptr = data; break;
        case NP_CommandName: command_ptr = data; break;
        case NP_Arguments: args_ptr = data; break;
        case NP_HomeDir:
            if (!data && !error) error = ERROR_NOT_IMPLEMENTED;
            directory = data;
            break;
        case NP_Priority:
            if ((LONG)data < -128 || (LONG)data > 127) error = ERROR_BAD_NUMBER;
            else spec.priority = (BYTE)(LONG)data;
            break;
        case NP_Cli:
            if (data > 1) error = ERROR_BAD_NUMBER;
            else spec.cli = data;
            break;
        case NP_CopyVars:
            if (data > 1) error = ERROR_BAD_NUMBER;
            else spec.copy_vars = data;
            break;
        case NP_ConsoleTask: case NP_WindowPtr:
            if (data && !error) error = ERROR_NOT_IMPLEMENTED;
            break;
        default:
            /* Includes NP_Entry and all guest exit callbacks/user data. */
            if (!error) error = ERROR_NOT_IMPLEMENTED;
            break;
        }
    }
    if (next != KAFFEINE_TAG_END) return fail(ERROR_BAD_NUMBER);
    if (error) return fail(error);
    if (!segment) return fail(ERROR_NOT_IMPLEMENTED);
    if ((name_ptr && !text_copy(ctx, name_ptr, name, sizeof(name))) ||
        (command_ptr && !text_copy(ctx, command_ptr, command, sizeof(command))) ||
        (args_ptr && !text_copy(ctx, args_ptr, arguments, sizeof(arguments))))
        return fail(ERROR_BAD_NUMBER);
    if (!command_ptr) memcpy(command, name, strlen(name) + 1);
    backend_length = GetVar((CONST_STRPTR)"KAFFEINE_CPU", (STRPTR)backend,
                            sizeof(backend), 0);
    if (backend_length <= 0) memcpy(backend, "moira", 6);
    else if (backend_length >= (LONG)sizeof(backend) ||
             (strcmp(backend, "jit") && strcmp(backend, "moira")))
        return fail(ERROR_BAD_NUMBER);
    spec.name = name;
    spec.command = command;
    spec.arguments = arguments;
    spec.backend = backend;

    if (!Kaffeine_SegmentTransferBegin(ctx, segment, &module)) return 0;
    if (!Kaffeine_LockTransferBegin(ctx, directory, &home))
    {
        saved_error = IoErr();
        Kaffeine_SegmentTransferEnd(ctx, &module, FALSE);
        return fail(saved_error);
    }
    spec.seglist = module.native;
    spec.home_dir = home.native;
    result = Kaffeine_DomainStartProcess(ctx, &spec);
    saved_error = IoErr();
    Kaffeine_LockTransferEnd(ctx, &home, result != 0);
    Kaffeine_SegmentTransferEnd(ctx, &module, result != 0);
    SetIoErr(saved_error);
    return result;
}
