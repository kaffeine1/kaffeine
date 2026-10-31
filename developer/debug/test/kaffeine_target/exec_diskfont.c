/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)
*/

#include <exec/types.h>
#include <graphics/text.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <diskfont/diskfont.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/diskfont.h>
#include <proto/intuition.h>

#include "m68ktest.h"

struct GfxBase *GfxBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct Library *DiskfontBase = NULL;

#define FIXTURE_PATH "PROGDIR:kaffeinedfprobe.font"
#define MISSING_PATH "PROGDIR:kaffeinedfprobe-missing.font"

#define GUARD_PRE  0x13579BDFUL
#define GUARD_MID  0x2468ACE0UL
#define GUARD_POST 0xFEDCBA98UL

struct GuardedTextAttr {
    ULONG pre_guard;
    struct TextAttr ta;
    ULONG mid_guard;
    char name_buf[40];
    ULONG post_guard;
};

static const ULONG expected_charloc[4] = {
    0x00000003UL,
    0x00030003UL,
    0x00060003UL,
    0x00090003UL
};

static const WORD expected_charspace[4] = { 4, 5, 4, 4 };
static const WORD expected_charkern[4]  = { 0, -1, 1, 0 };

static const UBYTE expected_bitmap[16] = {
    0x00, 0x00,
    0x5b, 0x60,
    0xb6, 0xa0,
    0xfb, 0x60,
    0xb6, 0xa0,
    0xbb, 0x60,
    0x00, 0x00,
    0x00, 0x00
};

static const char * const loc_names[4] = {
    "CharLoc[0]", "CharLoc[1]", "CharLoc[2]", "CharLoc[3]"
};

static const char * const space_names[4] = {
    "CharSpace[0]", "CharSpace[1]", "CharSpace[2]", "CharSpace[3]"
};

static const char * const kern_names[4] = {
    "CharKern[0]", "CharKern[1]", "CharKern[2]", "CharKern[3]"
};

static const char * const row_names[8] = {
    "glyph row 0", "glyph row 1", "glyph row 2", "glyph row 3",
    "glyph row 4", "glyph row 5", "glyph row 6", "glyph row 7"
};

static void copy_str_padded(char *dst, ULONG cap, const char *src)
{
    ULONG i = 0;

    while (i + 1 < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    if (cap > 0) {
        dst[i] = '\0';
        i++;
    }
    while (i < cap) {
        dst[i] = (char)0xA5;
        i++;
    }
}

static BOOL bytes_equal(const void *a, const void *b, ULONG len)
{
    const UBYTE *pa = (const UBYTE *)a;
    const UBYTE *pb = (const UBYTE *)b;
    ULONG i;

    for (i = 0; i < len; i++) {
        if (pa[i] != pb[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

static void init_guarded_attr(struct GuardedTextAttr *ga, const char *path)
{
    ga->pre_guard = GUARD_PRE;
    ga->mid_guard = GUARD_MID;
    ga->post_guard = GUARD_POST;
    copy_str_padded(ga->name_buf, (ULONG)sizeof(ga->name_buf), path);
    ga->ta.ta_Name = (STRPTR)ga->name_buf;
    ga->ta.ta_YSize = 8;
    ga->ta.ta_Style = 0;
    ga->ta.ta_Flags = FPF_DESIGNED;
}

static BOOL verify_guarded_attr(const struct GuardedTextAttr *ga,
                                const struct GuardedTextAttr *snap)
{
    if (ga->pre_guard != GUARD_PRE ||
        ga->mid_guard != GUARD_MID ||
        ga->post_guard != GUARD_POST) {
        return FALSE;
    }
    if (ga->ta.ta_Name != (STRPTR)ga->name_buf ||
        ga->ta.ta_YSize != 8 ||
        ga->ta.ta_Style != 0 ||
        ga->ta.ta_Flags != FPF_DESIGNED) {
        return FALSE;
    }
    return bytes_equal(ga, snap, (ULONG)sizeof(*ga));
}

static void run_tagged_case(const char *label, struct TagItem *tags, BOOL probe)
{
    struct TTextAttr tta;
    struct TTextAttr snapshot;
    struct TextFont *tf;
    LONG opened = 0;
    LONG ysize = 0;
    LONG xsize = 0;
    LONG flags = 0;

    tta.tta_Name = (STRPTR)FIXTURE_PATH;
    tta.tta_YSize = 8;
    tta.tta_Style = FSF_TAGGED;
    tta.tta_Flags = FPF_DESIGNED;
    tta.tta_Tags = tags;
    snapshot = tta;

    tf = OpenDiskFont((struct TextAttr *)&tta);
    if (tf != NULL) {
        opened = 1;
        ysize = (LONG)tf->tf_YSize;
        xsize = (LONG)tf->tf_XSize;
        flags = (LONG)tf->tf_Flags;
        CloseFont(tf);
    }

    if (probe) {
        Printf((STRPTR)"PROBE %s open=%ld ysize=%ld xsize=%ld flags=%lx\n",
               (LONG)label, opened, ysize, xsize, flags);
    } else {
        check(opened != 0, label);
        check_long("tagged font height", ysize, 8);
        check_long("tagged font width", xsize, 4);
        check(bytes_equal(&tta, &snapshot, sizeof(tta)), "tagged attributes unchanged");
    }
}

static LONG run_probe_mode(void)
{
    struct GuardedTextAttr ga;
    struct GuardedTextAttr snap;
    struct TextAttr missing_ta;
    struct TextFont *font1 = NULL;
    struct TextFont *font2 = NULL;
    struct TextFont *font3 = NULL;
    struct TextFont *missing_font = NULL;
    struct Window *win = NULL;
    struct TextFont *saved_font = NULL;
    struct TagItem dpi_tags[2];
    struct TagItem chain_tags[5];
    BOOL input_unchanged = FALSE;
    BOOL open1_ok = FALSE;
    BOOL open2_ok = FALSE;
    BOOL shared_ok = FALSE;
    BOOL reopen_ok = FALSE;
    BOOL win_ok = FALSE;
    LONG width1 = -1;
    LONG width2 = -1;

    init_guarded_attr(&ga, FIXTURE_PATH);
    snap = ga;

    font1 = OpenDiskFont(&ga.ta);
    open1_ok = (font1 != NULL) ? TRUE : FALSE;
    input_unchanged = verify_guarded_attr(&ga, &snap);

    Printf((STRPTR)"PROBE open1=%ld input_unchanged=%ld\n",
           (LONG)(open1_ok ? 1 : 0),
           (LONG)(input_unchanged ? 1 : 0));

    if (font1 != NULL) {
        const char *fname = (font1->tf_Message.mn_Node.ln_Name != NULL)
            ? (const char *)font1->tf_Message.mn_Node.ln_Name
            : "(null)";

        Printf((STRPTR)"PROBE name=%s\n", (LONG)fname);
        Printf((STRPTR)"PROBE metrics ysize=%ld xsize=%ld baseline=%ld boldsmear=%ld lochar=%ld hichar=%ld modulo=%ld\n",
               (LONG)font1->tf_YSize,
               (LONG)font1->tf_XSize,
               (LONG)font1->tf_Baseline,
               (LONG)font1->tf_BoldSmear,
               (LONG)font1->tf_LoChar,
               (LONG)font1->tf_HiChar,
               (LONG)font1->tf_Modulo);
        Printf((STRPTR)"PROBE style=%ld flags=%lx diskfont=%ld designed=%ld proportional=%ld accessors=%ld\n",
               (LONG)font1->tf_Style,
               (LONG)font1->tf_Flags,
               (LONG)((font1->tf_Flags & FPF_DISKFONT) ? 1 : 0),
               (LONG)((font1->tf_Flags & FPF_DESIGNED) ? 1 : 0),
               (LONG)((font1->tf_Flags & FPF_PROPORTIONAL) ? 1 : 0),
               (LONG)font1->tf_Accessors);
        Printf((STRPTR)"PROBE ptrs chardata=%ld charloc=%ld charspace=%ld charkern=%ld\n",
               (LONG)(font1->tf_CharData != NULL ? 1 : 0),
               (LONG)(font1->tf_CharLoc != NULL ? 1 : 0),
               (LONG)(font1->tf_CharSpace != NULL ? 1 : 0),
               (LONG)(font1->tf_CharKern != NULL ? 1 : 0));

        if (font1->tf_CharLoc != NULL) {
            const ULONG *loc = (const ULONG *)font1->tf_CharLoc;
            Printf((STRPTR)"PROBE charloc=%lx %lx %lx %lx\n",
                   (LONG)loc[0], (LONG)loc[1], (LONG)loc[2], (LONG)loc[3]);
        }
        if (font1->tf_CharSpace != NULL) {
            const WORD *sp = (const WORD *)font1->tf_CharSpace;
            Printf((STRPTR)"PROBE charspace=%ld %ld %ld %ld\n",
                   (LONG)sp[0], (LONG)sp[1], (LONG)sp[2], (LONG)sp[3]);
        }
        if (font1->tf_CharKern != NULL) {
            const WORD *kn = (const WORD *)font1->tf_CharKern;
            Printf((STRPTR)"PROBE charkern=%ld %ld %ld %ld\n",
                   (LONG)kn[0], (LONG)kn[1], (LONG)kn[2], (LONG)kn[3]);
        }
        if (font1->tf_CharData != NULL) {
            const UBYTE *bm = (const UBYTE *)font1->tf_CharData;
            Printf((STRPTR)"PROBE glyph_bytes=%lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx\n",
                   (LONG)bm[0], (LONG)bm[1], (LONG)bm[2], (LONG)bm[3],
                   (LONG)bm[4], (LONG)bm[5], (LONG)bm[6], (LONG)bm[7],
                   (LONG)bm[8], (LONG)bm[9], (LONG)bm[10], (LONG)bm[11],
                   (LONG)bm[12], (LONG)bm[13], (LONG)bm[14], (LONG)bm[15]);
        }

        font2 = OpenDiskFont(&ga.ta);
        open2_ok = (font2 != NULL) ? TRUE : FALSE;
        shared_ok = (font2 == font1) ? TRUE : FALSE;

        CloseFont(font1);
        font1 = NULL;

        if (font2 != NULL) {
            win = OpenWindowTags(NULL,
                                 WA_Left, 10L,
                                 WA_Top, 10L,
                                 WA_Width, 120L,
                                 WA_Height, 50L,
                                 TAG_DONE);
            if (win != NULL && win->RPort != NULL) {
                win_ok = TRUE;
                saved_font = win->RPort->Font;
                SetFont(win->RPort, font2);
                width1 = (LONG)TextLength(win->RPort, (STRPTR)"ABCZ", 4);
                Move(win->RPort, 8, 20);
                Text(win->RPort, (STRPTR)"ABCZ", 4);
                width2 = (LONG)TextLength(win->RPort, (STRPTR)"ABCZ", 4);
                if (saved_font != NULL) {
                    SetFont(win->RPort, saved_font);
                }
            }
            if (win != NULL) {
                CloseWindow(win);
                win = NULL;
            }
            CloseFont(font2);
            font2 = NULL;
        }

        font3 = OpenDiskFont(&ga.ta);
        if (font3 != NULL) {
            reopen_ok = TRUE;
            CloseFont(font3);
            font3 = NULL;
        }
    }

    Printf((STRPTR)"PROBE open2=%ld shared=%ld reopen=%ld\n",
           (LONG)(open2_ok ? 1 : 0),
           (LONG)(shared_ok ? 1 : 0),
           (LONG)(reopen_ok ? 1 : 0));
    Printf((STRPTR)"PROBE win=%ld width1=%ld width2=%ld\n",
           (LONG)(win_ok ? 1 : 0),
           (LONG)width1,
           (LONG)width2);

    missing_ta.ta_Name = (STRPTR)MISSING_PATH;
    missing_ta.ta_YSize = 8;
    missing_ta.ta_Style = 0;
    missing_ta.ta_Flags = FPF_DESIGNED;
    missing_font = OpenDiskFont(&missing_ta);
    Printf((STRPTR)"PROBE missing_is_null=%ld\n",
           (LONG)(missing_font == NULL ? 1 : 0));
    if (missing_font != NULL) {
        CloseFont(missing_font);
        missing_font = NULL;
    }

    run_tagged_case("tta_null", NULL, TRUE);

    dpi_tags[0].ti_Tag = TA_DeviceDPI;
    dpi_tags[0].ti_Data = (72UL << 16) | 72UL;
    dpi_tags[1].ti_Tag = TAG_DONE;
    dpi_tags[1].ti_Data = 0;
    run_tagged_case("tta_dpi", dpi_tags, TRUE);

    chain_tags[0].ti_Tag = TAG_IGNORE;
    chain_tags[0].ti_Data = 0x12345678UL;
    chain_tags[1].ti_Tag = TAG_SKIP;
    chain_tags[1].ti_Data = 1;
    chain_tags[2].ti_Tag = TAG_IGNORE;
    chain_tags[2].ti_Data = 0;
    chain_tags[3].ti_Tag = TAG_MORE;
    chain_tags[3].ti_Data = (ULONG)dpi_tags;
    chain_tags[4].ti_Tag = TAG_DONE;
    chain_tags[4].ti_Data = 0;
    run_tagged_case("tta_chain", chain_tags, TRUE);

    PutStr((STRPTR)"DISKFONT_PROBE_DONE\n");
    Flush(Output());
    return 0;
}

static LONG run_normal_mode(void)
{
    struct GuardedTextAttr ga;
    struct GuardedTextAttr snap;
    struct TextAttr missing_ta;
    struct TextFont *font1 = NULL;
    struct TextFont *font2 = NULL;
    struct TextFont *font3 = NULL;
    struct TextFont *missing_font = NULL;
    struct Window *win = NULL;
    struct TextFont *saved_font = NULL;
    ULONG req_flags = (ULONG)(FPF_DISKFONT | FPF_DESIGNED | FPF_PROPORTIONAL);
    ULONG i;

    init_guarded_attr(&ga, FIXTURE_PATH);
    snap = ga;

    font1 = OpenDiskFont(&ga.ta);
    check(font1 != NULL, "OpenDiskFont fixture returns non-NULL");
    check(verify_guarded_attr(&ga, &snap), "input TextAttr and filename unchanged");

    if (font1 != NULL) {
        BOOL has_name = (font1->tf_Message.mn_Node.ln_Name != NULL &&
                         font1->tf_Message.mn_Node.ln_Name[0] != '\0') ? TRUE : FALSE;

        check(has_name, "font node ln_Name non-NULL and non-empty");
        check_long("tf_YSize", (LONG)font1->tf_YSize, 8);
        check_long("tf_XSize", (LONG)font1->tf_XSize, 4);
        check_long("tf_Baseline", (LONG)font1->tf_Baseline, 6);
        check_long("tf_BoldSmear", (LONG)font1->tf_BoldSmear, 1);
        check_long("tf_LoChar", (LONG)font1->tf_LoChar, 65);
        check_long("tf_HiChar", (LONG)font1->tf_HiChar, 67);
        check_long("tf_Modulo", (LONG)font1->tf_Modulo, 2);
        check_long("tf_Style", (LONG)font1->tf_Style, 0);
        check_long("tf_Flags required bits",
                   (LONG)((ULONG)font1->tf_Flags & req_flags),
                   (LONG)req_flags);

        check(font1->tf_CharData != NULL, "tf_CharData non-NULL");
        check(font1->tf_CharLoc != NULL, "tf_CharLoc non-NULL");
        check(font1->tf_CharSpace != NULL, "tf_CharSpace non-NULL");
        check(font1->tf_CharKern != NULL, "tf_CharKern non-NULL");

        if (font1->tf_CharLoc != NULL) {
            const ULONG *loc = (const ULONG *)font1->tf_CharLoc;
            for (i = 0; i < 4; i++) {
                check_long(loc_names[i], (LONG)loc[i], (LONG)expected_charloc[i]);
            }
        }

        if (font1->tf_CharSpace != NULL) {
            const WORD *sp = (const WORD *)font1->tf_CharSpace;
            for (i = 0; i < 4; i++) {
                check_long(space_names[i], (LONG)sp[i], (LONG)expected_charspace[i]);
            }
        }

        if (font1->tf_CharKern != NULL) {
            const WORD *kn = (const WORD *)font1->tf_CharKern;
            for (i = 0; i < 4; i++) {
                check_long(kern_names[i], (LONG)kn[i], (LONG)expected_charkern[i]);
            }
        }

        if (font1->tf_CharData != NULL) {
            const UBYTE *bm = (const UBYTE *)font1->tf_CharData;
            for (i = 0; i < 8; i++) {
                LONG got_row = ((LONG)bm[i * 2] << 8) | (LONG)bm[i * 2 + 1];
                LONG exp_row = ((LONG)expected_bitmap[i * 2] << 8) |
                               (LONG)expected_bitmap[i * 2 + 1];
                check_long(row_names[i], got_row, exp_row);
            }
        }

        font2 = OpenDiskFont(&ga.ta);
        check(font2 != NULL, "second OpenDiskFont returns non-NULL");
        check(font2 == font1, "two concurrent opens share TextFont instance");

        CloseFont(font1);
        font1 = NULL;

        if (font2 != NULL) {
            BOOL second_intact = (font2->tf_YSize == 8 &&
                                  font2->tf_CharData != NULL &&
                                  font2->tf_CharLoc != NULL &&
                                  font2->tf_CharSpace != NULL &&
                                  font2->tf_CharKern != NULL &&
                                  ((const ULONG *)font2->tf_CharLoc)[1] == 0x00030003UL &&
                                  ((const WORD *)font2->tf_CharSpace)[1] == 5 &&
                                  ((const WORD *)font2->tf_CharKern)[1] == -1 &&
                                  ((const UBYTE *)font2->tf_CharData)[2] == 0x5b)
                                 ? TRUE : FALSE;
            check(second_intact, "second open remains readable after closing first");
            {
                struct TextAttr memory_attr = ga.ta;
                struct TextFont *memory_font;
                memory_attr.ta_Name = (STRPTR)"kaffeinedfprobe.font";
                memory_font = OpenFont(&memory_attr);
                check(memory_font != NULL, "graphics OpenFont finds loaded disk font");
                check(memory_font == font2, "graphics and diskfont share font identity");
                if (memory_font != NULL)
                    CloseFont(memory_font);
            }

            win = OpenWindowTags(NULL,
                                 WA_Left, 10L,
                                 WA_Top, 10L,
                                 WA_Width, 120L,
                                 WA_Height, 50L,
                                 TAG_DONE);
            check(win != NULL && win->RPort != NULL, "OpenWindowTags window and RastPort valid");
            if (win != NULL && win->RPort != NULL) {
                LONG w1;
                LONG w2;

                saved_font = win->RPort->Font;
                SetFont(win->RPort, font2);
                check(win->RPort->Font == font2, "SetFont binds fixture font to window RastPort");

                w1 = (LONG)TextLength(win->RPort, (STRPTR)"ABCZ", 4);
                Move(win->RPort, 8, 20);
                Text(win->RPort, (STRPTR)"ABCZ", 4);
                w2 = (LONG)TextLength(win->RPort, (STRPTR)"ABCZ", 4);

                check_long("TextLength for ABCZ matches AmigaOS", w1, 17);
                check_long("TextLength for ABCZ is repeatable", w2, w1);

                if (saved_font != NULL) {
                    SetFont(win->RPort, saved_font);
                }
            }
            if (win != NULL) {
                CloseWindow(win);
                win = NULL;
            }

            CloseFont(font2);
            font2 = NULL;
        }

        font3 = OpenDiskFont(&ga.ta);
        check(font3 != NULL, "reopen after closing prior opens succeeds");
        if (font3 != NULL) {
            check(font3->tf_YSize == 8 && font3->tf_CharData != NULL,
                  "reopened font retains valid metrics and bitmap pointer");
            CloseFont(font3);
            font3 = NULL;
        }
    }

    missing_ta.ta_Name = (STRPTR)MISSING_PATH;
    missing_ta.ta_YSize = 8;
    missing_ta.ta_Style = 0;
    missing_ta.ta_Flags = FPF_DESIGNED;
    missing_font = OpenDiskFont(&missing_ta);
    check(missing_font == NULL, "missing disk font returns NULL");
    if (missing_font != NULL) {
        CloseFont(missing_font);
        missing_font = NULL;
    }

    {
        struct TagItem dpi[2] = {
            { TA_DeviceDPI, (72UL << 16) | 72UL }, { TAG_DONE, 0 }
        };
        struct TagItem chain[5] = {
            { TAG_IGNORE, 0x12345678UL }, { TAG_SKIP, 1 },
            { TAG_IGNORE, 0 }, { TAG_MORE, (ULONG)dpi }, { TAG_DONE, 0 }
        };
        struct TagItem dpi_snapshot[2];
        struct TagItem chain_snapshot[5];
        for (i = 0; i < 2; i++) dpi_snapshot[i] = dpi[i];
        for (i = 0; i < 5; i++) chain_snapshot[i] = chain[i];
        run_tagged_case("tagged NULL list opens", NULL, FALSE);
        run_tagged_case("tagged scalar DPI opens", dpi, FALSE);
        run_tagged_case("tagged valid chain opens", chain, FALSE);
        check(bytes_equal(dpi, dpi_snapshot, sizeof(dpi)) &&
              bytes_equal(chain, chain_snapshot, sizeof(chain)), "guest tag arrays unchanged");
    }
    return test_done("exec_diskfont");
}

int main(int argc, char **argv)
{
    LONG rc = 10;

    (void)argv;

    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 0);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 0);
    DiskfontBase = OpenLibrary((STRPTR)"diskfont.library", 0);

    if (GfxBase == NULL || IntuitionBase == NULL || DiskfontBase == NULL) {
        if (argc > 1) {
            Printf((STRPTR)"PROBE lib_open_failed gfx=%ld intuition=%ld diskfont=%ld\n",
                   (LONG)(GfxBase != NULL ? 1 : 0),
                   (LONG)(IntuitionBase != NULL ? 1 : 0),
                   (LONG)(DiskfontBase != NULL ? 1 : 0));
            Flush(Output());
        } else {
            check(GfxBase != NULL, "OpenLibrary graphics.library");
            check(IntuitionBase != NULL, "OpenLibrary intuition.library");
            check(DiskfontBase != NULL, "OpenLibrary diskfont.library");
            rc = test_done("exec_diskfont");
        }
    } else if (argc > 1) {
        rc = run_probe_mode();
    } else {
        rc = run_normal_mode();
    }

    if (DiskfontBase != NULL) {
        CloseLibrary(DiskfontBase);
        DiskfontBase = NULL;
    }
    if (IntuitionBase != NULL) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
    if (GfxBase != NULL) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }

    Flush(Output());
    return (int)rc;
}
