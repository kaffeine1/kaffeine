/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_PROCESS_H
#define KAFFEINE_PROCESS_H

#include <exec/types.h>
#include <dos/bptr.h>

struct KaffeineContext;

#define KAFFEINE_PROCESS_MAX_NAME 255
#define KAFFEINE_PROCESS_MAX_ARGS 4095
#define KAFFEINE_PROCESS_MAX_STACK (64 * 1024)

/* All pointers and BPTRs here are trusted native values. The DOS parser must
 * finish typed segment/lock validation before DomainStartProcess. Backend must
 * be "moira" or "jit". A zero home_dir means omit NP_HomeDir (inherit).
 * Startup copies the strings and guest code into the caller's shared arena.
 * Each guest Process has its own CPU, requested stack and native DOS broker;
 * its queues and message pointers remain in that same arena. No broker or
 * native code pointer is exposed to the guest.
 *
 * Nonzero return consumes the seglist and home_dir native references. Zero
 * leaves both original references with the caller. The parser must commit or
 * restore its transfer leases accordingly. Failed startup may retain private
 * copies until the stopped driver's recovery drain acknowledges cleanup.
 * The broker owns a separate home lock, streams, CLI and local variables.
 * Guest code may outlive the root Process and its original native caller.
 */
struct KaffeineProcessStart
{
    BPTR seglist;
    ULONG stack_size;
    const char *name, *command, *arguments;
    BPTR home_dir;
    BOOL cli, copy_vars;
    BYTE priority;
    const char *backend;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Typed launch in the caller's shared 68k domain. Success consumes both
   native source references after copying the module and publishing its CPU. */
ULONG Kaffeine_DomainStartProcess(struct KaffeineContext *,
                                 const struct KaffeineProcessStart *);
/* Private compiled broker jobs. No guest CPU execution or callback. */
IPTR Kaffeine_ProcessInitGuestDos(struct KaffeineContext *, void *);
IPTR Kaffeine_ProcessDropGuestDos(struct KaffeineContext *, void *);

#ifdef __cplusplus
}
#endif
#endif
