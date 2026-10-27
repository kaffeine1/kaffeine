/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <devices/conunit.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/console.h>
#include <proto/dos.h>

#include "m68ktest.h"

struct Device *ConsoleDevice = NULL;

#define GUARD_PRE   4
#define NORM_BUFLEN 16
#define GUARD_POST  4
#define NORM_TOTAL  (GUARD_PRE + NORM_BUFLEN + GUARD_POST)

#define PROBE_PRE    2
#define PROBE_CAP    8
#define PROBE_POST   2
#define PROBE_TOTAL  (PROBE_PRE + PROBE_CAP + PROBE_POST)

#define GUARD_BYTE  0xA5
#define FILL_BYTE   0x5A

struct UniversalKeyCase {
    UWORD code;
    UBYTE expected;
    const char *count_desc;
    const char *byte_desc;
    const char *guard_desc;
};

struct ProbeSingleCase {
    const char *name;
    UBYTE ie_class;
    UWORD ie_code;
    UWORD ie_qual;
    LONG buf_len;
};

static void clear_bytes(void *ptr, ULONG len)
{
    volatile UBYTE *p = (volatile UBYTE *)ptr;
    ULONG i;

    for (i = 0; i < len; i++)
        p[i] = 0;
}

static void copy_bytes(void *dst, const void *src, ULONG len)
{
    volatile UBYTE *d = (volatile UBYTE *)dst;
    const volatile UBYTE *s = (const volatile UBYTE *)src;
    ULONG i;

    for (i = 0; i < len; i++)
        d[i] = s[i];
}

static BOOL bytes_equal(const void *a, const void *b, ULONG len)
{
    const volatile UBYTE *pa = (const volatile UBYTE *)a;
    const volatile UBYTE *pb = (const volatile UBYTE *)b;
    ULONG i;

    for (i = 0; i < len; i++) {
        if (pa[i] != pb[i])
            return FALSE;
    }
    return TRUE;
}

static void init_rawkey_event(struct InputEvent *ie, UBYTE ie_class, UWORD code, UWORD qual)
{
    clear_bytes(ie, sizeof(*ie));
    ie->ie_Class = ie_class;
    ie->ie_Code = code;
    ie->ie_Qualifier = qual;
}

static void prepare_guarded_buf(UBYTE *raw, ULONG total, ULONG pre, ULONG usable_len)
{
    volatile UBYTE *p = (volatile UBYTE *)raw;
    ULONG i;

    for (i = 0; i < total; i++)
        p[i] = GUARD_BYTE;
    for (i = 0; i < usable_len; i++)
        p[pre + i] = FILL_BYTE;
}

static BOOL verify_guards(const UBYTE *raw, ULONG total, ULONG pre, ULONG usable_len)
{
    const volatile UBYTE *p = (const volatile UBYTE *)raw;
    ULONG i;

    for (i = 0; i < pre; i++) {
        if (p[i] != GUARD_BYTE)
            return FALSE;
    }
    for (i = pre + usable_len; i < total; i++) {
        if (p[i] != GUARD_BYTE)
            return FALSE;
    }
    return TRUE;
}

static LONG convert_checked(UWORD code, UBYTE *out_first, BOOL *out_integrity)
{
    struct InputEvent ie;
    struct InputEvent ie_ref;
    UBYTE raw_buf[NORM_TOTAL];
    LONG actual;
    BOOL ie_ok;
    BOOL guard_ok;

    init_rawkey_event(&ie, IECLASS_RAWKEY, code, 0);
    copy_bytes(&ie_ref, &ie, sizeof(ie));
    prepare_guarded_buf(raw_buf, NORM_TOTAL, GUARD_PRE, NORM_BUFLEN);

    actual = RawKeyConvert(&ie, (STRPTR)&raw_buf[GUARD_PRE], NORM_BUFLEN, NULL);

    ie_ok = bytes_equal(&ie, &ie_ref, sizeof(ie));
    guard_ok = verify_guards(raw_buf, NORM_TOTAL, GUARD_PRE, NORM_BUFLEN);

    *out_first = raw_buf[GUARD_PRE];
    *out_integrity = (ie_ok && guard_ok) ? TRUE : FALSE;
    return actual;
}

static int run_normal_mode(void)
{
    static const struct UniversalKeyCase ukeys[] = {
        { 0x40, 0x20, "space 0x40 count",     "space 0x40 byte",     "space 0x40 ie/guards" },
        { 0x42, 0x09, "tab 0x42 count",       "tab 0x42 byte",       "tab 0x42 ie/guards" },
        { 0x44, 0x0d, "return 0x44 count",    "return 0x44 byte",    "return 0x44 ie/guards" },
        { 0x45, 0x1b, "escape 0x45 count",    "escape 0x45 byte",    "escape 0x45 ie/guards" },
        { 0x41, 0x08, "backspace 0x41 count", "backspace 0x41 byte", "backspace 0x41 ie/guards" },
        { 0x46, 0x7f, "delete 0x46 count",    "delete 0x46 byte",    "delete 0x46 ie/guards" }
    };
    struct IOStdReq req1;
    struct IOStdReq req2;
    struct MsgPort *port;
    struct IOStdReq *ior;
    struct Device *saved_base = NULL;
    LONG err1;
    LONG err2;
    LONG err3;
    LONG actual;
    UBYTE first_byte;
    BOOL integrity;
    ULONG i;

    check_long("CONU_LIBRARY constant", (LONG)CONU_LIBRARY, -1L);

    clear_bytes(&req1, sizeof(req1));
    err1 = (LONG)OpenDevice("console.device", (ULONG)CONU_LIBRARY, (struct IORequest *)&req1, 0);
    check_long("open1 status", err1, 0L);
    check(req1.io_Device != NULL, "open1 io_Device non-NULL");
    check_long("open1 io_Error", (LONG)req1.io_Error, 0L);
    check(req1.io_Message.mn_ReplyPort == NULL, "open1 mn_ReplyPort remains NULL");

    if (err1 == 0 && req1.io_Device != NULL) {
        ConsoleDevice = req1.io_Device;
        saved_base = req1.io_Device;

        for (i = 0; i < (ULONG)(sizeof(ukeys) / sizeof(ukeys[0])); i++) {
            actual = convert_checked(ukeys[i].code, &first_byte, &integrity);
            check_long(ukeys[i].count_desc, actual, 1L);
            check_long(ukeys[i].byte_desc, (LONG)first_byte, (LONG)ukeys[i].expected);
            check(integrity, ukeys[i].guard_desc);
        }

        actual = convert_checked((UWORD)(0x40 | IECODE_UP_PREFIX), &first_byte, &integrity);
        check_long("keyup 0xc0 count", actual, 0L);
        check(integrity, "keyup 0xc0 ie/guards");

        clear_bytes(&req2, sizeof(req2));
        err2 = (LONG)OpenDevice("console.device", (ULONG)CONU_LIBRARY, (struct IORequest *)&req2, 0);
        check_long("open2 status", err2, 0L);
        check(req2.io_Device != NULL, "open2 io_Device non-NULL");
        check(req2.io_Device == req1.io_Device, "open2 shares io_Device base");

        CloseDevice((struct IORequest *)&req1);

        if (err2 == 0 && req2.io_Device != NULL) {
            ConsoleDevice = req2.io_Device;
            actual = convert_checked(0x40, &first_byte, &integrity);
            check_long("post-close1 space count", actual, 1L);
            check_long("post-close1 space byte", (LONG)first_byte, 0x20L);
            check(integrity, "post-close1 space ie/guards");

            CloseDevice((struct IORequest *)&req2);
        }
        ConsoleDevice = NULL;
    }

    port = CreateMsgPort();
    check(port != NULL, "CreateMsgPort non-NULL");
    if (port != NULL) {
        ior = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
        check(ior != NULL, "CreateIORequest non-NULL");
        if (ior != NULL) {
            err3 = (LONG)OpenDevice("console.device", (ULONG)CONU_LIBRARY, (struct IORequest *)ior, 0);
            check_long("open3 dynamic status", err3, 0L);
            check(ior->io_Device != NULL && ior->io_Device == saved_base, "open3 matches device base");

            if (err3 == 0 && ior->io_Device != NULL) {
                ConsoleDevice = ior->io_Device;
                actual = convert_checked(0x44, &first_byte, &integrity);
                check_long("open3 return count", actual, 1L);
                check(first_byte == 0x0d && integrity, "open3 return byte and guards");

                CloseDevice((struct IORequest *)ior);
                ConsoleDevice = NULL;
            }
            DeleteIORequest((struct IORequest *)ior);
        }
        DeleteMsgPort(port);
    }

    Flush(Output());
    return (int)test_done("exec_console");
}

static void print_probe_row(const char *name, LONG buf_len, LONG ret, BOOL ie_ok, BOOL guard_ok, const UBYTE *raw)
{
    Printf("PROBE_CASE %s len=%ld ret=%ld ie_ok=%ld guard_ok=%ld pre=%lx,%lx buf=%lx,%lx,%lx,%lx,%lx,%lx,%lx,%lx post=%lx,%lx\n",
           (LONG)name,
           buf_len,
           ret,
           (LONG)ie_ok,
           (LONG)guard_ok,
           (LONG)raw[0], (LONG)raw[1],
           (LONG)raw[2], (LONG)raw[3], (LONG)raw[4], (LONG)raw[5],
           (LONG)raw[6], (LONG)raw[7], (LONG)raw[8], (LONG)raw[9],
           (LONG)raw[10], (LONG)raw[11]);
}

static void run_probe_single(const struct ProbeSingleCase *c)
{
    struct InputEvent ie;
    struct InputEvent ie_ref;
    UBYTE raw_buf[PROBE_TOTAL];
    LONG ret;
    BOOL ie_ok;
    BOOL guard_ok;

    init_rawkey_event(&ie, c->ie_class, c->ie_code, c->ie_qual);
    copy_bytes(&ie_ref, &ie, sizeof(ie));
    prepare_guarded_buf(raw_buf, PROBE_TOTAL, PROBE_PRE, (ULONG)c->buf_len);

    ret = RawKeyConvert(&ie, (STRPTR)&raw_buf[PROBE_PRE], c->buf_len, NULL);

    ie_ok = bytes_equal(&ie, &ie_ref, sizeof(ie));
    guard_ok = verify_guards(raw_buf, PROBE_TOTAL, PROBE_PRE, (ULONG)c->buf_len);
    print_probe_row(c->name, c->buf_len, ret, ie_ok, guard_ok, raw_buf);
}

static void run_probe_chain(const char *name, LONG buf_len)
{
    struct InputEvent ie1;
    struct InputEvent ie2;
    struct InputEvent ie1_ref;
    struct InputEvent ie2_ref;
    UBYTE raw_buf[PROBE_TOTAL];
    LONG ret;
    BOOL ie_ok;
    BOOL guard_ok;

    init_rawkey_event(&ie1, IECLASS_RAWKEY, 0x40, 0);
    init_rawkey_event(&ie2, IECLASS_RAWKEY, 0x44, 0);
    ie1.ie_NextEvent = &ie2;

    copy_bytes(&ie1_ref, &ie1, sizeof(ie1));
    copy_bytes(&ie2_ref, &ie2, sizeof(ie2));
    prepare_guarded_buf(raw_buf, PROBE_TOTAL, PROBE_PRE, (ULONG)buf_len);

    ret = RawKeyConvert(&ie1, (STRPTR)&raw_buf[PROBE_PRE], buf_len, NULL);

    ie_ok = (bytes_equal(&ie1, &ie1_ref, sizeof(ie1)) &&
             bytes_equal(&ie2, &ie2_ref, sizeof(ie2))) ? TRUE : FALSE;
    guard_ok = verify_guards(raw_buf, PROBE_TOTAL, PROBE_PRE, (ULONG)buf_len);
    print_probe_row(name, buf_len, ret, ie_ok, guard_ok, raw_buf);
}

static void print_closed_ioreq(const char *tag, const struct IOStdReq *req)
{
    Printf("%s dev=%lx dev_null=%ld dev_m1=%ld unit=%lx unit_m1=%ld err=%ld flags=%lx mn_len=%ld\n",
           (LONG)tag,
           (LONG)(ULONG)req->io_Device,
           (LONG)(req->io_Device == NULL),
           (LONG)(req->io_Device == (struct Device *)-1L),
           (LONG)(ULONG)req->io_Unit,
           (LONG)(req->io_Unit == (struct Unit *)-1L),
           (LONG)req->io_Error,
           (LONG)req->io_Flags,
           (LONG)req->io_Message.mn_Length);
}

static int run_probe_mode(void)
{
    static const struct ProbeSingleCase single_cases[] = {
        { "space_0x40",     IECLASS_RAWKEY, 0x40, 0,                    8 },
        { "tab_0x42",       IECLASS_RAWKEY, 0x42, 0,                    8 },
        { "return_0x44",    IECLASS_RAWKEY, 0x44, 0,                    8 },
        { "escape_0x45",    IECLASS_RAWKEY, 0x45, 0,                    8 },
        { "bs_0x41",        IECLASS_RAWKEY, 0x41, 0,                    8 },
        { "del_0x46",       IECLASS_RAWKEY, 0x46, 0,                    8 },
        { "raw_0x20_none",  IECLASS_RAWKEY, 0x20, 0,                    8 },
        { "raw_0x20_shift", IECLASS_RAWKEY, 0x20, IEQUALIFIER_LSHIFT,   8 },
        { "raw_0x20_caps",  IECLASS_RAWKEY, 0x20, IEQUALIFIER_CAPSLOCK, 8 },
        { "raw_0x20_ctrl",  IECLASS_RAWKEY, 0x20, IEQUALIFIER_CONTROL,  8 },
        { "keyup_0xc0",     IECLASS_RAWKEY, (UWORD)(0x40 | IECODE_UP_PREFIX), 0, 8 },
        { "ieclass_null",   IECLASS_NULL,   0x40, 0,                    8 },
        { "space_len0",     IECLASS_RAWKEY, 0x40, 0,                    0 },
        { "space_len1",     IECLASS_RAWKEY, 0x40, 0,                    1 },
        { "up_0x4c_len8",   IECLASS_RAWKEY, 0x4c, 0,                    8 },
        { "up_0x4c_len1",   IECLASS_RAWKEY, 0x4c, 0,                    1 },
        { "up_0x4c_len0",   IECLASS_RAWKEY, 0x4c, 0,                    0 }
    };
    struct IOStdReq req1;
    struct IOStdReq req2;
    struct MsgPort *port;
    struct IOStdReq *ior;
    LONG err1;
    LONG err2;
    LONG err3;
    ULONG i;

    clear_bytes(&req1, sizeof(req1));
    err1 = (LONG)OpenDevice("console.device", (ULONG)CONU_LIBRARY, (struct IORequest *)&req1, 0);
    Printf("PROBE_OPEN1 err=%ld dev_nz=%ld unit=%lx io_err=%ld flags=%lx mn_len=%ld type=%ld ver=%ld rev=%ld\n",
           err1,
           (LONG)(req1.io_Device != NULL),
           (LONG)(ULONG)req1.io_Unit,
           (LONG)req1.io_Error,
           (LONG)req1.io_Flags,
           (LONG)req1.io_Message.mn_Length,
           req1.io_Device ? (LONG)req1.io_Device->dd_Library.lib_Node.ln_Type : -1L,
           req1.io_Device ? (LONG)req1.io_Device->dd_Library.lib_Version : -1L,
           req1.io_Device ? (LONG)req1.io_Device->dd_Library.lib_Revision : -1L);

    if (err1 == 0 && req1.io_Device != NULL) {
        ConsoleDevice = req1.io_Device;

        clear_bytes(&req2, sizeof(req2));
        err2 = (LONG)OpenDevice("console.device", (ULONG)CONU_LIBRARY, (struct IORequest *)&req2, 0);
        Printf("PROBE_OPEN2 err=%ld dev_nz=%ld same_base=%ld same_unit=%ld unit=%lx io_err=%ld flags=%lx mn_len=%ld\n",
               err2,
               (LONG)(req2.io_Device != NULL),
               (LONG)(req2.io_Device == req1.io_Device),
               (LONG)(req2.io_Unit == req1.io_Unit),
               (LONG)(ULONG)req2.io_Unit,
               (LONG)req2.io_Error,
               (LONG)req2.io_Flags,
               (LONG)req2.io_Message.mn_Length);

        if (err2 == 0 && req2.io_Device != NULL) {
            CloseDevice((struct IORequest *)&req2);
            print_closed_ioreq("PROBE_CLOSE2", &req2);
        }

        for (i = 0; i < (ULONG)(sizeof(single_cases) / sizeof(single_cases[0])); i++)
            run_probe_single(&single_cases[i]);

        run_probe_chain("chain_sp_ret_len1", 1);
        run_probe_chain("chain_sp_ret_len2", 2);
        run_probe_chain("chain_sp_ret_len8", 8);

        CloseDevice((struct IORequest *)&req1);
        ConsoleDevice = NULL;
        print_closed_ioreq("PROBE_CLOSE1", &req1);
    }

    port = CreateMsgPort();
    if (port != NULL) {
        ior = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
        if (ior != NULL) {
            err3 = (LONG)OpenDevice("console.device", (ULONG)CONU_LIBRARY, (struct IORequest *)ior, 0);
            Printf("PROBE_DYN_OPEN port_nz=%ld ior_nz=%ld err=%ld dev_nz=%ld unit=%lx io_err=%ld mn_len=%ld\n",
                   1L,
                   1L,
                   err3,
                   (LONG)(ior->io_Device != NULL),
                   (LONG)(ULONG)ior->io_Unit,
                   (LONG)ior->io_Error,
                   (LONG)ior->io_Message.mn_Length);
            if (err3 == 0 && ior->io_Device != NULL) {
                CloseDevice((struct IORequest *)ior);
                print_closed_ioreq("PROBE_DYN_CLOSE", ior);
            }
            DeleteIORequest((struct IORequest *)ior);
        }
        DeleteMsgPort(port);
    }

    PutStr("CONSOLE_PROBE_DONE\n");
    Flush(Output());
    return 0;
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1)
        return run_probe_mode();
    return run_normal_mode();
}
