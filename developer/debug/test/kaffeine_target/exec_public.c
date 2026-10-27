/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Guest public ports, refused native objects and Exec allocation vectors.
    Compile the optional fixture with a native AROS compiler, never as 68k.
*/
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stddef.h>

#define NATIVE_PORT_NAME "kaffeine.exec_public.native"

#ifdef KAFFEINE_NATIVE_PORT_FIXTURE

int main(void)
{
    struct MsgPort port;

    memset(&port, 0, sizeof(port));
    port.mp_Node.ln_Name = NATIVE_PORT_NAME;
    port.mp_Flags = PA_IGNORE;
    Forbid();
    if (FindPort(NATIVE_PORT_NAME))
    {
        Permit();
        PutStr("Native port fixture already exists.\n");
        return 10;
    }
    AddPort(&port);
    if (FindPort(NATIVE_PORT_NAME) != &port)
    {
        RemPort(&port);
        Permit();
        return 10;
    }
    Permit();
    PutStr("Native port kaffeine.exec_public.native exists. Run exec_public now; Ctrl-C removes it.\n");
    Flush(Output());
    Wait(SIGBREAKF_CTRL_C);
    RemPort(&port);
    return 0;
}

#else

#include "m68ktest.h"

static struct MsgPort port;
static struct Message message;
static struct Node resource;
struct EntryRequest
{
    struct Node node;
    UWORD count;
    struct MemEntry entries[3];
};
static struct EntryRequest request;

typedef char check_node_size[sizeof(struct Node) == 14 ? 1 : -1];
typedef char check_port_size[sizeof(struct MsgPort) == 34 ? 1 : -1];
typedef char check_entry_offset[offsetof(struct MemList, ml_ME) == 16 ? 1 : -1];
typedef char check_request_offset[offsetof(struct EntryRequest, entries) == 16 ? 1 : -1];

static void public_checks(void)
{
    memset(&port, 0xA5, sizeof(port));
    port.mp_Node.ln_Name = "kaffeine.exec_public.guest";
    port.mp_Node.ln_Pri = -7;
    port.mp_Flags = PA_IGNORE;
    port.mp_SigBit = 0;
    port.mp_SigTask = FindTask(NULL);
    AddPort(&port);
    check(port.mp_Node.ln_Type == NT_MSGPORT &&
          port.mp_MsgList.lh_Head == (struct Node *)&port.mp_MsgList.lh_Tail &&
          port.mp_MsgList.lh_Tail == NULL &&
          port.mp_MsgList.lh_TailPred == (struct Node *)&port.mp_MsgList.lh_Head,
          "AddPort() initializes the guest message list and node type");
    check(FindPort("kaffeine.exec_public.guest") == &port &&
          FindPort("kaffeine.exec_public.guest") == &port,
          "FindPort() returns the same published guest port");
    RemPort(&port);
    check(FindPort("kaffeine.exec_public.guest") == NULL,
          "RemPort() removes the guest name");
    check(FindPort(NATIVE_PORT_NAME) == NULL,
          "FindPort() cannot see the native companion's port (start its fixture first)");
    check(FindResident("exec.library") == NULL,
          "FindResident() does not expose a native resident");
    check(OpenResource("dos.resource") == NULL,
          "OpenResource() does not expose a native resource");
    memset(&resource, 0, sizeof(resource));
    resource.ln_Name = "kaffeine.exec_public.resource";
    AddResource(&resource);
    check(OpenResource(resource.ln_Name) == NULL,
          "AddResource() does not publish a guest structure to native Exec");
    RemResource(&resource);
    CacheClearE(&port, sizeof(port), CACRF_ClearI | CACRF_ClearD);
    check(port.mp_Node.ln_Name && !strcmp(port.mp_Node.ln_Name, "kaffeine.exec_public.guest"),
          "CacheClearE() returns without changing guest bytes");
}

static void allocation_checks(void)
{
    ULONG before = AvailMem(MEMF_ANY), i;
    UBYTE *block = AllocMem(512, MEMF_PUBLIC), *location, *absolute;
    struct MemList *list;

    check(block != NULL, "AllocMem() provides a block for the absolute allocation probe");
    if (block)
    {
        memset(block, 0x6D, 512);
        location = block + 64;
        FreeMem(block, 512);
        absolute = AllocAbs(33, location + 3);
        check(absolute == location, "AllocAbs() inside a freed block returns its aligned start");
        if (absolute)
        {
            BOOL intact = TRUE;
            for (i = 0; i < 40; i++)
                if (absolute[i] != 0x6D) intact = FALSE;
            check(intact, "AllocAbs() preserves the rounded allocation's contents");
            check(AllocAbs(8, absolute) == NULL, "AllocAbs() refuses an occupied range");
            FreeMem(absolute, 33 + (ULONG)location + 3 - (ULONG)absolute);
        }
    }
    check(AvailMem(MEMF_ANY) == before, "FreeMem() releases the absolute allocation");

    memset(&request, 0, sizeof(request));
    request.count = 3;
    request.entries[0].me_Reqs = MEMF_PUBLIC;
    request.entries[0].me_Length = 17;
    request.entries[1].me_Reqs = MEMF_PUBLIC | MEMF_CLEAR;
    request.entries[1].me_Length = 25;
    /* AROS permits zero-length entries, which retain a NULL address. */
    list = AllocEntry((struct MemList *)&request);
    check(list && !((ULONG)list & 0x80000000UL), "AllocEntry() returns a guest MemList");
    if (list && !((ULONG)list & 0x80000000UL))
    {
        BOOL clear = TRUE;
        check(list != (struct MemList *)&request && list->ml_NumEntries == 3 &&
              list->ml_ME[0].me_Length == 17 && list->ml_ME[0].me_Addr &&
              list->ml_ME[1].me_Length == 25 && list->ml_ME[1].me_Addr &&
              !list->ml_ME[2].me_Length && !list->ml_ME[2].me_Addr,
              "AllocEntry() copies lengths and fills the entry addresses");
        if (list->ml_ME[1].me_Addr)
            for (i = 0; i < 25; i++)
                if (((UBYTE *)list->ml_ME[1].me_Addr)[i]) clear = FALSE;
        check(clear, "AllocEntry() honors each entry's MEMF_CLEAR");
        FreeEntry(list);
    }
    check(AvailMem(MEMF_ANY) == before, "FreeEntry() releases every entry and the MemList");
    request.entries[1].me_Reqs = MEMF_FAST | MEMF_CLEAR;
    request.entries[1].me_Length = 0x7FFFFFFFUL;
    list = AllocEntry((struct MemList *)&request);
    check((ULONG)list == (0x80000000UL | MEMF_FAST | MEMF_CLEAR),
          "AllocEntry() returns the failing requirements with bit 31 set");
    if (list && !((ULONG)list & 0x80000000UL)) FreeEntry(list);
    check(AvailMem(MEMF_ANY) == before, "AllocEntry() cleans up a partial allocation failure");
}

int main(int argc, char **argv)
{
    /* Optional containment probe, separate from the native passing run.
       A NULL destination must stop with rc 20 before native Exec sees it.
       Valid ordinary guest queues are covered by exec_messages. */
    if (argc == 2 && !strcmp(argv[1], "putmsg"))
    {
        struct MsgPort *found;

        memset(&port, 0, sizeof(port));
        port.mp_Node.ln_Name = "kaffeine.exec_public.putmsg";
        port.mp_Flags = PA_IGNORE;
        AddPort(&port);
        found = FindPort("kaffeine.exec_public.putmsg");
        check(found == &port, "FindPort() returns the published guest port");
        memset(&message, 0, sizeof(message));
        message.mn_Length = sizeof(message);
        PutMsg(NULL, &message);
        check(FALSE, "PutMsg() to a NULL port unexpectedly returned");
        RemPort(&port);
        return test_done("exec_public");
    }
    public_checks();
    allocation_checks();
    return test_done("exec_public");
}

#endif
