/*
 * Copyright (C) 2026, The AROS Development Team.
 *
 * Standalone Amiga 68000 C regression and observation test for Exec messages:
 * PutMsg, GetMsg, ReplyMsg, WaitPort.
 */

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/nodes.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "m68ktest.h"

#ifndef NewList
VOID NewList(struct List *list);
#endif

struct TestMsg
{
    struct Message msg;
    ULONG guard1;
    LONG id;
    LONG data;
    ULONG guard2;
};

int main(int argc, char **argv)
{
    BOOL probe = (argc > 1) ? TRUE : FALSE;
    struct Task *self = FindTask(NULL);
    struct MsgPort *dest_port = NULL;
    struct MsgPort *reply_port = NULL;
    struct MsgPort local_port;
    struct TestMsg m1;
    struct TestMsg m2;
    struct TestMsg m3;
    const char *name1 = "msg_one";
    const char *name2 = "msg_two";
    const char *name3 = "msg_three";
    ULONG dest_mask = 0;
    ULONG reply_mask = 0;
    ULONG both_mask = 0;

    struct Message *wp1 = NULL;
    struct Message *wp2 = NULL;
    struct Message *got1 = NULL;
    struct Message *got2 = NULL;
    struct Message *got3 = NULL;
    struct Message *rg1 = NULL;
    struct Message *rg2 = NULL;
    struct Message *rg3 = NULL;
    struct Message *null_got = NULL;
    struct Message *null_extra = NULL;
    struct Message *reuse_got = NULL;
    struct Message *reuse_extra = NULL;
    struct Message *ig1 = NULL;
    struct Message *ig2 = NULL;
    struct Message *ig3 = NULL;
    struct Message *loc_wp = NULL;
    struct Message *loc_got = NULL;
    struct Message *loc_extra = NULL;

    LONG ports_created = 0;
    LONG dest_ln_type = 0;
    LONG reply_ln_type = 0;
    LONG dest_flags_action = 0;
    LONG reply_flags_action = 0;
    LONG sigbits_valid_distinct = 0;
    LONG sigtask_is_self = 0;
    LONG dest_init_empty = 0;
    LONG reply_init_empty = 0;
    LONG dest_list_lh_type = 0;

    LONG m1_type_init = 0;
    LONG m1_type_after_put = 0;
    LONG m2_type_after_put = 0;
    LONG m1_type_after_get = 0;
    LONG m2_type_after_get = 0;
    LONG m1_type_after_reply = 0;
    LONG m2_type_after_reply = 0;
    LONG m1_type_after_reply_get = 0;
    LONG m3_type_after_put = 0;
    LONG m3_type_before_null_reply = 0;
    LONG m3_type_after_null_reply = 0;

    LONG sig_before_put1 = 0;
    LONG sig_after_put1 = 0;
    LONG other_sig_preserved_put1 = 0;
    LONG sig_cleared_mid_queue = 0;
    LONG sig_after_put2_nonempty = 0;
    LONG other_sig_preserved_put2 = 0;
    LONG sig_before_waitport = 0;
    LONG sig_after_waitport1 = 0;
    LONG sig_after_waitport2 = 0;
    LONG sig_after_get1 = 0;
    LONG sig_after_get2_empty = 0;
    LONG sig_after_get3_null = 0;

    LONG two_msg_links_ok = 0;
    LONG m1_fields_intact = 0;
    LONG m2_fields_intact = 0;
    LONG waitport_peek_first_ok = 0;
    LONG waitport_links_unchanged = 0;
    LONG one_msg_left_links_ok = 0;
    LONG dest_fifo_order_ok = 0;
    LONG dest_drained_sentinels_ok = 0;

    LONG reply_sig_before_reply = 0;
    LONG reply_sig_after_reply1 = 0;
    LONG reply_links_two_ok = 0;
    LONG reply_fifo_order_ok = 0;
    LONG reply_payload_intact = 0;
    LONG reply_drained_sentinels_ok = 0;

    LONG null_reply_no_sig = 0;
    LONG null_reply_queues_nothing = 0;
    LONG null_reply_payload_intact = 0;

    LONG reuse_type_after_put = 0;
    LONG reuse_sig_ok = 0;
    LONG reuse_roundtrip_ok = 0;

    LONG pa_ignore_no_sig = 0;
    LONG pa_ignore_type_ok = 0;
    LONG pa_ignore_fifo_ok = 0;
    LONG pa_signal_restored = 0;

    LONG local_newlist_init_empty = 0;
    LONG local_port_no_sig = 0;
    LONG local_port_roundtrip_ok = 0;
    LONG local_port_guards_ok = 0;
    LONG local_port_drained_empty = 0;
    LONG changed_action_peek = 0, changed_action_get = 0;
    LONG changed_action_empty = 0, changed_action_signal = 0;

    (void)argv;

    dest_port = CreateMsgPort();
    reply_port = CreateMsgPort();

    if (dest_port == NULL || reply_port == NULL)
    {
        if (reply_port != NULL)
            DeleteMsgPort(reply_port);
        if (dest_port != NULL)
            DeleteMsgPort(dest_port);

        if (probe)
        {
            Printf("PROBE ports_created=%ld\n", (LONG)0);
            PutStr("MESSAGES_PROBE_DONE\n");
            Flush(Output());
            return 0;
        }

        check(FALSE, "CreateMsgPort destination and reply ports");
        Flush(Output());
        return test_done("exec_messages");
    }

    ports_created = (dest_port != reply_port) ? 1 : 0;
    dest_ln_type = (LONG)dest_port->mp_Node.ln_Type;
    reply_ln_type = (LONG)reply_port->mp_Node.ln_Type;
    dest_flags_action = (LONG)(dest_port->mp_Flags & PF_ACTION);
    reply_flags_action = (LONG)(reply_port->mp_Flags & PF_ACTION);
    sigbits_valid_distinct = (dest_port->mp_SigBit < 32 &&
                              reply_port->mp_SigBit < 32 &&
                              dest_port->mp_SigBit != reply_port->mp_SigBit) ? 1 : 0;
    sigtask_is_self = (dest_port->mp_SigTask == self &&
                       reply_port->mp_SigTask == self) ? 1 : 0;
    dest_init_empty = (dest_port->mp_MsgList.lh_Head == (struct Node *)&dest_port->mp_MsgList.lh_Tail &&
                       dest_port->mp_MsgList.lh_Tail == NULL &&
                       dest_port->mp_MsgList.lh_TailPred == (struct Node *)&dest_port->mp_MsgList.lh_Head) ? 1 : 0;
    reply_init_empty = (reply_port->mp_MsgList.lh_Head == (struct Node *)&reply_port->mp_MsgList.lh_Tail &&
                        reply_port->mp_MsgList.lh_Tail == NULL &&
                        reply_port->mp_MsgList.lh_TailPred == (struct Node *)&reply_port->mp_MsgList.lh_Head) ? 1 : 0;
    dest_list_lh_type = (LONG)dest_port->mp_MsgList.lh_Type;

    dest_mask = 1UL << dest_port->mp_SigBit;
    reply_mask = 1UL << reply_port->mp_SigBit;
    both_mask = dest_mask | reply_mask;

    /* Initialize three distinct messages */
    memset(&m1, 0, sizeof(m1));
    m1.msg.mn_Node.ln_Type = 0;
    m1.msg.mn_Node.ln_Pri = -5;
    m1.msg.mn_Node.ln_Name = (char *)name1;
    m1.msg.mn_ReplyPort = reply_port;
    m1.msg.mn_Length = (UWORD)sizeof(struct TestMsg);
    m1.guard1 = 0x55AA1111UL;
    m1.id = 101;
    m1.data = 0x12340001L;
    m1.guard2 = 0xAA551111UL;

    memset(&m2, 0, sizeof(m2));
    m2.msg.mn_Node.ln_Type = 0;
    m2.msg.mn_Node.ln_Pri = 20;
    m2.msg.mn_Node.ln_Name = (char *)name2;
    m2.msg.mn_ReplyPort = reply_port;
    m2.msg.mn_Length = (UWORD)sizeof(struct TestMsg);
    m2.guard1 = 0x55AA2222UL;
    m2.id = 202;
    m2.data = 0x12340002L;
    m2.guard2 = 0xAA552222UL;

    memset(&m3, 0, sizeof(m3));
    m3.msg.mn_Node.ln_Type = 0;
    m3.msg.mn_Node.ln_Pri = 7;
    m3.msg.mn_Node.ln_Name = (char *)name3;
    m3.msg.mn_ReplyPort = reply_port;
    m3.msg.mn_Length = (UWORD)sizeof(struct TestMsg);
    m3.guard1 = 0x55AA3333UL;
    m3.id = 303;
    m3.data = 0x12340003L;
    m3.guard2 = 0xAA553333UL;

    m1_type_init = (LONG)m1.msg.mn_Node.ln_Type;

    /*
     * Isolate owned signal bits: clear dest_mask and set reply_mask as a
     * known unrelated owned signal bit to verify PutMsg does not clear it.
     */
    SetSignal(reply_mask, both_mask);
    sig_before_put1 = (SetSignal(0UL, 0UL) & dest_mask) ? 1 : 0;

    PutMsg(dest_port, &m1.msg);
    m1_type_after_put = (LONG)m1.msg.mn_Node.ln_Type;
    sig_after_put1 = (SetSignal(0UL, 0UL) & dest_mask) ? 1 : 0;
    other_sig_preserved_put1 = (SetSignal(0UL, 0UL) & reply_mask) ? 1 : 0;

    /* Clear dest_mask while m1 remains queued, then enqueue m2 */
    SetSignal(0UL, dest_mask);
    sig_cleared_mid_queue = ((SetSignal(0UL, 0UL) & dest_mask) == 0) ? 1 : 0;

    PutMsg(dest_port, &m2.msg);
    m2_type_after_put = (LONG)m2.msg.mn_Node.ln_Type;
    sig_after_put2_nonempty = (SetSignal(0UL, 0UL) & dest_mask) ? 1 : 0;
    other_sig_preserved_put2 = (SetSignal(0UL, 0UL) & reply_mask) ? 1 : 0;

    /* Clear reply_mask now; leave dest_mask set to observe WaitPort/GetMsg */
    SetSignal(0UL, reply_mask);

    two_msg_links_ok = (dest_port->mp_MsgList.lh_Head == &m1.msg.mn_Node &&
                        dest_port->mp_MsgList.lh_Tail == NULL &&
                        dest_port->mp_MsgList.lh_TailPred == &m2.msg.mn_Node &&
                        m1.msg.mn_Node.ln_Pred == (struct Node *)&dest_port->mp_MsgList.lh_Head &&
                        m1.msg.mn_Node.ln_Succ == &m2.msg.mn_Node &&
                        m2.msg.mn_Node.ln_Pred == &m1.msg.mn_Node &&
                        m2.msg.mn_Node.ln_Succ == (struct Node *)&dest_port->mp_MsgList.lh_Tail) ? 1 : 0;

    m1_fields_intact = (m1.guard1 == 0x55AA1111UL &&
                        m1.id == 101 &&
                        m1.data == 0x12340001L &&
                        m1.guard2 == 0xAA551111UL &&
                        m1.msg.mn_Node.ln_Pri == -5 &&
                        m1.msg.mn_Node.ln_Name == (char *)name1 &&
                        m1.msg.mn_ReplyPort == reply_port &&
                        m1.msg.mn_Length == (UWORD)sizeof(struct TestMsg)) ? 1 : 0;

    m2_fields_intact = (m2.guard1 == 0x55AA2222UL &&
                        m2.id == 202 &&
                        m2.data == 0x12340002L &&
                        m2.guard2 == 0xAA552222UL &&
                        m2.msg.mn_Node.ln_Pri == 20 &&
                        m2.msg.mn_Node.ln_Name == (char *)name2 &&
                        m2.msg.mn_ReplyPort == reply_port &&
                        m2.msg.mn_Length == (UWORD)sizeof(struct TestMsg)) ? 1 : 0;

    /* Repeated WaitPort on nonempty queue */
    sig_before_waitport = (SetSignal(0UL, 0UL) & dest_mask) ? 1 : 0;
    wp1 = WaitPort(dest_port);
    sig_after_waitport1 = (SetSignal(0UL, 0UL) & dest_mask) ? 1 : 0;
    wp2 = WaitPort(dest_port);
    sig_after_waitport2 = (SetSignal(0UL, 0UL) & dest_mask) ? 1 : 0;

    waitport_peek_first_ok = (wp1 == &m1.msg && wp2 == &m1.msg) ? 1 : 0;
    waitport_links_unchanged = (dest_port->mp_MsgList.lh_Head == &m1.msg.mn_Node &&
                                dest_port->mp_MsgList.lh_Tail == NULL &&
                                dest_port->mp_MsgList.lh_TailPred == &m2.msg.mn_Node &&
                                m1.msg.mn_Node.ln_Succ == &m2.msg.mn_Node &&
                                m2.msg.mn_Node.ln_Pred == &m1.msg.mn_Node) ? 1 : 0;

    /* Drain destination queue with GetMsg */
    got1 = GetMsg(dest_port);
    m1_type_after_get = (LONG)m1.msg.mn_Node.ln_Type;
    sig_after_get1 = (SetSignal(0UL, 0UL) & dest_mask) ? 1 : 0;
    one_msg_left_links_ok = (dest_port->mp_MsgList.lh_Head == &m2.msg.mn_Node &&
                             dest_port->mp_MsgList.lh_Tail == NULL &&
                             dest_port->mp_MsgList.lh_TailPred == &m2.msg.mn_Node &&
                             m2.msg.mn_Node.ln_Pred == (struct Node *)&dest_port->mp_MsgList.lh_Head &&
                             m2.msg.mn_Node.ln_Succ == (struct Node *)&dest_port->mp_MsgList.lh_Tail) ? 1 : 0;

    got2 = GetMsg(dest_port);
    m2_type_after_get = (LONG)m2.msg.mn_Node.ln_Type;
    sig_after_get2_empty = (SetSignal(0UL, 0UL) & dest_mask) ? 1 : 0;

    got3 = GetMsg(dest_port);
    sig_after_get3_null = (SetSignal(0UL, 0UL) & dest_mask) ? 1 : 0;

    dest_fifo_order_ok = (got1 == &m1.msg && got2 == &m2.msg && got3 == NULL) ? 1 : 0;
    dest_drained_sentinels_ok = (dest_port->mp_MsgList.lh_Head == (struct Node *)&dest_port->mp_MsgList.lh_Tail &&
                                 dest_port->mp_MsgList.lh_Tail == NULL &&
                                 dest_port->mp_MsgList.lh_TailPred == (struct Node *)&dest_port->mp_MsgList.lh_Head) ? 1 : 0;

    SetSignal(0UL, both_mask);

    /* Reply removed messages to reply_port and verify FIFO, NT_REPLYMSG, signal */
    reply_sig_before_reply = (SetSignal(0UL, 0UL) & reply_mask) ? 1 : 0;
    ReplyMsg(&m1.msg);
    m1_type_after_reply = (LONG)m1.msg.mn_Node.ln_Type;
    reply_sig_after_reply1 = (SetSignal(0UL, 0UL) & reply_mask) ? 1 : 0;

    ReplyMsg(&m2.msg);
    m2_type_after_reply = (LONG)m2.msg.mn_Node.ln_Type;
    reply_links_two_ok = (reply_port->mp_MsgList.lh_Head == &m1.msg.mn_Node &&
                          reply_port->mp_MsgList.lh_Tail == NULL &&
                          reply_port->mp_MsgList.lh_TailPred == &m2.msg.mn_Node &&
                          m1.msg.mn_Node.ln_Succ == &m2.msg.mn_Node &&
                          m2.msg.mn_Node.ln_Pred == &m1.msg.mn_Node) ? 1 : 0;

    rg1 = GetMsg(reply_port);
    m1_type_after_reply_get = (LONG)m1.msg.mn_Node.ln_Type;
    rg2 = GetMsg(reply_port);
    rg3 = GetMsg(reply_port);

    reply_fifo_order_ok = (rg1 == &m1.msg && rg2 == &m2.msg && rg3 == NULL) ? 1 : 0;
    reply_payload_intact = (m1.guard1 == 0x55AA1111UL &&
                            m1.id == 101 &&
                            m1.data == 0x12340001L &&
                            m1.guard2 == 0xAA551111UL &&
                            m2.guard1 == 0x55AA2222UL &&
                            m2.id == 202 &&
                            m2.data == 0x12340002L &&
                            m2.guard2 == 0xAA552222UL) ? 1 : 0;
    reply_drained_sentinels_ok = (reply_port->mp_MsgList.lh_Head == (struct Node *)&reply_port->mp_MsgList.lh_Tail &&
                                  reply_port->mp_MsgList.lh_Tail == NULL &&
                                  reply_port->mp_MsgList.lh_TailPred == (struct Node *)&reply_port->mp_MsgList.lh_Head) ? 1 : 0;

    SetSignal(0UL, both_mask);

    /* Message with mn_ReplyPort = NULL: put, get, reply; observe resulting type */
    m3.msg.mn_ReplyPort = NULL;
    PutMsg(dest_port, &m3.msg);
    m3_type_after_put = (LONG)m3.msg.mn_Node.ln_Type;
    null_got = GetMsg(dest_port);
    null_extra = GetMsg(dest_port);
    m3_type_before_null_reply = (LONG)m3.msg.mn_Node.ln_Type;

    SetSignal(0UL, both_mask);
    ReplyMsg(&m3.msg);
    m3_type_after_null_reply = (LONG)m3.msg.mn_Node.ln_Type;
    null_reply_no_sig = ((SetSignal(0UL, 0UL) & both_mask) == 0) ? 1 : 0;
    null_reply_queues_nothing = (null_got == &m3.msg &&
                                 null_extra == NULL &&
                                 GetMsg(reply_port) == NULL &&
                                 GetMsg(dest_port) == NULL) ? 1 : 0;
    null_reply_payload_intact = (m3.guard1 == 0x55AA3333UL &&
                                 m3.id == 303 &&
                                 m3.data == 0x12340003L &&
                                 m3.guard2 == 0xAA553333UL) ? 1 : 0;

    /* Reuse a removed/replied message in another enqueue/get round */
    m1.data = 0x56780001L;
    m1.msg.mn_ReplyPort = reply_port;
    SetSignal(0UL, both_mask);
    PutMsg(dest_port, &m1.msg);
    reuse_type_after_put = (LONG)m1.msg.mn_Node.ln_Type;
    reuse_sig_ok = (SetSignal(0UL, 0UL) & dest_mask) ? 1 : 0;
    reuse_got = GetMsg(dest_port);
    reuse_extra = GetMsg(dest_port);
    reuse_roundtrip_ok = (reuse_got == &m1.msg &&
                          reuse_extra == NULL &&
                          m1.guard1 == 0x55AA1111UL &&
                          m1.id == 101 &&
                          m1.data == 0x56780001L &&
                          m1.guard2 == 0xAA551111UL) ? 1 : 0;
    SetSignal(0UL, both_mask);

    /* Change owned empty dest_port to PA_IGNORE, enqueue two messages, check FIFO & no signal */
    dest_port->mp_Flags = (UBYTE)((dest_port->mp_Flags & ~PF_ACTION) | PA_IGNORE);
    m1.msg.mn_Node.ln_Type = 0;
    m2.msg.mn_Node.ln_Type = 0;
    SetSignal(0UL, both_mask);
    PutMsg(dest_port, &m1.msg);
    PutMsg(dest_port, &m2.msg);
    pa_ignore_no_sig = ((SetSignal(0UL, 0UL) & both_mask) == 0) ? 1 : 0;
    pa_ignore_type_ok = (m1.msg.mn_Node.ln_Type == NT_MESSAGE &&
                         m2.msg.mn_Node.ln_Type == NT_MESSAGE) ? 1 : 0;
    ig1 = GetMsg(dest_port);
    ig2 = GetMsg(dest_port);
    ig3 = GetMsg(dest_port);
    pa_ignore_fifo_ok = (ig1 == &m1.msg && ig2 == &m2.msg && ig3 == NULL) ? 1 : 0;

    /* Restore PA_SIGNAL before cleanup */
    dest_port->mp_Flags = (UBYTE)((dest_port->mp_Flags & ~PF_ACTION) | PA_SIGNAL);
    pa_signal_restored = ((dest_port->mp_Flags & PF_ACTION) == PA_SIGNAL) ? 1 : 0;

    /* Manually initialized local MsgPort with NewList and PA_IGNORE */
    memset(&local_port, 0, sizeof(local_port));
    local_port.mp_Node.ln_Type = NT_MSGPORT;
    local_port.mp_Flags = PA_IGNORE;
    local_port.mp_SigBit = 0;
    local_port.mp_SigTask = NULL;
    NewList(&local_port.mp_MsgList);
    local_port.mp_MsgList.lh_Type = NT_MESSAGE;

    local_newlist_init_empty = (local_port.mp_MsgList.lh_Head == (struct Node *)&local_port.mp_MsgList.lh_Tail &&
                                local_port.mp_MsgList.lh_Tail == NULL &&
                                local_port.mp_MsgList.lh_TailPred == (struct Node *)&local_port.mp_MsgList.lh_Head) ? 1 : 0;

    m3.msg.mn_Node.ln_Type = 0;
    m3.msg.mn_ReplyPort = reply_port;
    SetSignal(0UL, both_mask);
    PutMsg(&local_port, &m3.msg);
    local_port_no_sig = ((SetSignal(0UL, 0UL) & both_mask) == 0) ? 1 : 0;
    loc_wp = WaitPort(&local_port);
    loc_got = GetMsg(&local_port);
    loc_extra = GetMsg(&local_port);

    local_port_roundtrip_ok = (loc_wp == &m3.msg &&
                               loc_got == &m3.msg &&
                               loc_extra == NULL &&
                               m3.msg.mn_Node.ln_Type == NT_MESSAGE) ? 1 : 0;
    local_port_guards_ok = (m3.guard1 == 0x55AA3333UL &&
                            m3.id == 303 &&
                            m3.data == 0x12340003L &&
                            m3.guard2 == 0xAA553333UL) ? 1 : 0;
    local_port_drained_empty = (local_port.mp_MsgList.lh_Head == (struct Node *)&local_port.mp_MsgList.lh_Tail &&
                                local_port.mp_MsgList.lh_Tail == NULL &&
                                local_port.mp_MsgList.lh_TailPred == (struct Node *)&local_port.mp_MsgList.lh_Head) ? 1 : 0;

    /* Queue inspection does not perform the port's arrival action. */
    {
        ULONG before_single, after_single;
        struct Message *peeked, *removed, *last;
        local_port.mp_Flags = PA_IGNORE;
        local_port.mp_SigTask = self;
        local_port.mp_SigBit = SIGB_SINGLE;
        before_single = SetSignal(0UL, 0UL) & SIGF_SINGLE;
        PutMsg(&local_port, &m3.msg);
        local_port.mp_Flags = PA_SIGNAL;
        peeked = WaitPort(&local_port);
        removed = GetMsg(&local_port);
        last = GetMsg(&local_port);
        after_single = SetSignal(0UL, 0UL) & SIGF_SINGLE;
        changed_action_peek = peeked == &m3.msg;
        changed_action_get = removed == &m3.msg;
        changed_action_empty = last == NULL &&
            local_port.mp_MsgList.lh_Head == (struct Node *)&local_port.mp_MsgList.lh_Tail &&
            local_port.mp_MsgList.lh_Tail == NULL &&
            local_port.mp_MsgList.lh_TailPred == (struct Node *)&local_port.mp_MsgList.lh_Head;
        changed_action_signal = before_single == after_single;
    }

    /* Clean up owned signals and the two ports returned by CreateMsgPort */
    SetSignal(0UL, both_mask);
    DeleteMsgPort(reply_port);
    DeleteMsgPort(dest_port);

    if (probe)
    {
        Printf("PROBE ports_created=%ld dest_type=%ld reply_type=%ld\n",
               (LONG)ports_created, (LONG)dest_ln_type, (LONG)reply_ln_type);
        Printf("PROBE dest_action=%ld reply_action=%ld sigbits_ok=%ld sigtask_self=%ld\n",
               (LONG)dest_flags_action, (LONG)reply_flags_action,
               (LONG)sigbits_valid_distinct, (LONG)sigtask_is_self);
        Printf("PROBE init_empty_dest=%ld init_empty_reply=%ld list_lh_type=%ld\n",
               (LONG)dest_init_empty, (LONG)reply_init_empty, (LONG)dest_list_lh_type);
        Printf("PROBE type_init=%ld type_put1=%ld type_put2=%ld type_get1=%ld type_get2=%ld\n",
               (LONG)m1_type_init, (LONG)m1_type_after_put, (LONG)m2_type_after_put,
               (LONG)m1_type_after_get, (LONG)m2_type_after_get);
        Printf("PROBE type_reply1=%ld type_reply2=%ld type_reply_get1=%ld\n",
               (LONG)m1_type_after_reply, (LONG)m2_type_after_reply,
               (LONG)m1_type_after_reply_get);
        Printf("PROBE type_m3_put=%ld type_m3_before_null_reply=%ld type_m3_after_null_reply=%ld\n",
               (LONG)m3_type_after_put, (LONG)m3_type_before_null_reply,
               (LONG)m3_type_after_null_reply);
        Printf("PROBE sig_pre_put1=%ld sig_post_put1=%ld other_sig1=%ld sig_mid_clr=%ld sig_post_put2=%ld other_sig2=%ld\n",
               (LONG)sig_before_put1, (LONG)sig_after_put1, (LONG)other_sig_preserved_put1,
               (LONG)sig_cleared_mid_queue, (LONG)sig_after_put2_nonempty,
               (LONG)other_sig_preserved_put2);
        Printf("PROBE sig_pre_wp=%ld sig_post_wp1=%ld sig_post_wp2=%ld sig_post_get1=%ld sig_post_get2=%ld sig_post_get3=%ld\n",
               (LONG)sig_before_waitport, (LONG)sig_after_waitport1, (LONG)sig_after_waitport2,
               (LONG)sig_after_get1, (LONG)sig_after_get2_empty, (LONG)sig_after_get3_null);
        Printf("PROBE links_two=%ld m1_intact=%ld m2_intact=%ld wp_first=%ld wp_links=%ld links_one=%ld\n",
               (LONG)two_msg_links_ok, (LONG)m1_fields_intact, (LONG)m2_fields_intact,
               (LONG)waitport_peek_first_ok, (LONG)waitport_links_unchanged,
               (LONG)one_msg_left_links_ok);
        Printf("PROBE dest_fifo=%ld dest_empty_sentinels=%ld\n",
               (LONG)dest_fifo_order_ok, (LONG)dest_drained_sentinels_ok);
        Printf("PROBE reply_sig_pre=%ld reply_sig_post=%ld reply_links=%ld reply_fifo=%ld reply_payload=%ld reply_empty=%ld\n",
               (LONG)reply_sig_before_reply, (LONG)reply_sig_after_reply1,
               (LONG)reply_links_two_ok, (LONG)reply_fifo_order_ok,
               (LONG)reply_payload_intact, (LONG)reply_drained_sentinels_ok);
        Printf("PROBE null_reply_no_sig=%ld null_reply_empty=%ld null_reply_payload=%ld\n",
               (LONG)null_reply_no_sig, (LONG)null_reply_queues_nothing,
               (LONG)null_reply_payload_intact);
        Printf("PROBE reuse_type=%ld reuse_sig=%ld reuse_ok=%ld\n",
               (LONG)reuse_type_after_put, (LONG)reuse_sig_ok, (LONG)reuse_roundtrip_ok);
        Printf("PROBE pa_ignore_no_sig=%ld pa_ignore_type=%ld pa_ignore_fifo=%ld pa_signal_restored=%ld\n",
               (LONG)pa_ignore_no_sig, (LONG)pa_ignore_type_ok,
               (LONG)pa_ignore_fifo_ok, (LONG)pa_signal_restored);
        Printf("PROBE local_init_empty=%ld local_no_sig=%ld local_roundtrip=%ld local_guards=%ld local_drained_empty=%ld\n",
               (LONG)local_newlist_init_empty, (LONG)local_port_no_sig,
               (LONG)local_port_roundtrip_ok, (LONG)local_port_guards_ok,
               (LONG)local_port_drained_empty);
        Printf("PROBE changed_action_peek=%ld changed_action_get=%ld changed_action_empty=%ld changed_action_signal=%ld\n",
               changed_action_peek, changed_action_get, changed_action_empty, changed_action_signal);
        PutStr("MESSAGES_PROBE_DONE\n");
        Flush(Output());
        return 0;
    }

    check(ports_created != 0, "CreateMsgPort created two distinct ports");
    check_long("dest port ln_Type", dest_ln_type, (LONG)NT_MSGPORT);
    check_long("reply port ln_Type", reply_ln_type, (LONG)NT_MSGPORT);
    check(sigbits_valid_distinct != 0, "ports have valid distinct signal bits");
    check_long("dest port PA_SIGNAL", dest_flags_action, (LONG)PA_SIGNAL);
    check_long("reply port PA_SIGNAL", reply_flags_action, (LONG)PA_SIGNAL);
    check(sigtask_is_self != 0, "ports mp_SigTask equals FindTask(NULL)");
    check(dest_init_empty != 0 && reply_init_empty != 0, "new ports start with empty list sentinels");

    check_long("m1 ln_Type after PutMsg", m1_type_after_put, (LONG)NT_MESSAGE);
    check_long("m2 ln_Type after PutMsg", m2_type_after_put, (LONG)NT_MESSAGE);
    check(two_msg_links_ok != 0, "two queued messages head/tail/pred/succ links");
    check(m1_fields_intact != 0 && m2_fields_intact != 0, "queued messages preserve payload, guards and metadata");

    check(sig_before_put1 == 0 && sig_after_put1 != 0 && other_sig_preserved_put1 != 0,
          "first PutMsg signals destination bit without clearing unrelated owned bit");
    check(sig_cleared_mid_queue != 0 && sig_after_put2_nonempty != 0 && other_sig_preserved_put2 != 0,
          "second PutMsg on nonempty queue signals again and preserves unrelated bit");

    check(waitport_peek_first_ok != 0 && waitport_links_unchanged != 0,
          "repeated WaitPort on nonempty queue returns first message without modifying queue");
    check(one_msg_left_links_ok != 0 && dest_fifo_order_ok != 0,
          "GetMsg drains in FIFO order and returns NULL when empty");
    check(dest_drained_sentinels_ok != 0, "drained destination port matches empty list sentinels");

    check(reply_sig_before_reply == 0 && reply_sig_after_reply1 != 0, "ReplyMsg signals reply port");
    check_long("m1 ln_Type after ReplyMsg", m1_type_after_reply, (LONG)NT_REPLYMSG);
    check_long("m2 ln_Type after ReplyMsg", m2_type_after_reply, (LONG)NT_REPLYMSG);
    check(reply_links_two_ok != 0 && reply_fifo_order_ok != 0 &&
          reply_payload_intact != 0 && reply_drained_sentinels_ok != 0,
          "reply port preserves FIFO order, payload and empty list sentinels");

    Printf("INFO null_reply_ln_type=%ld\n", (LONG)m3_type_after_null_reply);
    check(m3_type_after_put == (LONG)NT_MESSAGE &&
          null_reply_no_sig != 0 &&
          null_reply_queues_nothing != 0 &&
          null_reply_payload_intact != 0,
          "ReplyMsg with NULL ReplyPort sends no reply and preserves payload");

    check(reuse_type_after_put == (LONG)NT_MESSAGE && reuse_sig_ok != 0 && reuse_roundtrip_ok != 0,
          "reused message round-trips with updated payload and NT_MESSAGE");

    check(pa_ignore_no_sig != 0 && pa_ignore_type_ok != 0 &&
          pa_ignore_fifo_ok != 0 && pa_signal_restored != 0,
          "PA_IGNORE port queues in FIFO without signaling and restores PA_SIGNAL");

    check(local_newlist_init_empty != 0 && local_port_no_sig != 0 &&
          local_port_roundtrip_ok != 0 && local_port_guards_ok != 0 &&
          local_port_drained_empty != 0,
          "local NewList PA_IGNORE port round-trips and preserves guards");

    check(changed_action_peek && changed_action_get && changed_action_empty && changed_action_signal,
          "nonempty WaitPort and GetMsg ignore changed arrival metadata and preserve reserved signal state");
    Flush(Output());
    return test_done("exec_messages");
}
