/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    strgclass copies texts and numbers into the buffer of a string gadget
    no further than its MaxChars, gives the lowest LONG its digits, and
    takes MaxChars and the buffers only at creation. The buffers of the
    test that the class writes into have guard bytes on both sides, which
    no copy may touch; one more buffer only shows that the class does not
    take it after creation. Output in the form of the kaffeine target tests:
    a line per check and a summary.
*/

#include <proto/exec.h>
#include <proto/intuition.h>
#include <intuition/classes.h>
#include <intuition/gadgetclass.h>
#include <intuition/intuition.h>
#include <intuition/sghooks.h>
#include <stdio.h>
#include <string.h>

#define GUARD 8
#define GUARDBYTE 0xA5

static int checks, failures;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

/* A buffer of size bytes with GUARD bytes before and after it */
static UBYTE *guarded(UBYTE *area, ULONG size)
{
    memset(area, GUARDBYTE, size + 2 * GUARD);
    return area + GUARD;
}

static int intact(const UBYTE *buffer, ULONG size)
{
    ULONG i;

    for (i = 1; i <= GUARD; i++)
        if (buffer[-(LONG)i] != GUARDBYTE || buffer[size - 1 + i] != GUARDBYTE)
            return 0;
    return 1;
}

int main(void)
{
    static UBYTE area[3][6 + 2 * GUARD], one[1 + 2 * GUARD], other[32];
    struct Gadget *g;
    struct StringInfo *si;
    UBYTE *buffer, *undo, *work;
    STRPTR own;

    /* The class's own buffers */
    g = (struct Gadget *)NewObject(NULL, STRGCLASS, STRINGA_MaxChars, 6,
                                   STRINGA_TextVal, (IPTR)"Hello, world", TAG_DONE);
    check(g != NULL, "a strgclass gadget of MaxChars 6");
    if (g)
    {
        si = (struct StringInfo *)g->SpecialInfo;
        own = si->Buffer;
        check(si->MaxChars == 6 && strcmp((char *)own, "Hello") == 0,
              "a longer STRINGA_TextVal at creation keeps its first 5 characters");
        SetAttrs(g, STRINGA_TextVal, (IPTR)"A much longer text", TAG_DONE);
        check(strcmp((char *)own, "A muc") == 0, "so does a longer STRINGA_TextVal set later");
        DisposeObject((Object *)g);
    }

    /* Buffers of the test, with guard bytes */
    buffer = guarded(area[0], 6);
    undo = guarded(area[1], 6);
    work = guarded(area[2], 6);
    g = (struct Gadget *)NewObject(NULL, STRGCLASS, STRINGA_MaxChars, 6,
                                   STRINGA_Buffer, (IPTR)buffer, STRINGA_UndoBuffer, (IPTR)undo,
                                   STRINGA_WorkBuffer, (IPTR)work,
                                   STRINGA_TextVal, (IPTR)"Hello, world", TAG_DONE);
    check(g != NULL, "a strgclass gadget with buffers of 6 bytes between guard bytes");
    if (g)
    {
        si = (struct StringInfo *)g->SpecialInfo;
        check(si->Buffer == (STRPTR)buffer && strcmp((char *)buffer, "Hello") == 0 &&
              intact(buffer, 6), "a longer text at creation stays within the buffer");

        SetAttrs(g, STRINGA_TextVal, (IPTR)"A much longer text", TAG_DONE);
        check(strcmp((char *)buffer, "A muc") == 0 && intact(buffer, 6),
              "a longer text set later stays within the buffer");

        SetAttrs(g, STRINGA_TextVal, (IPTR)"Short", TAG_DONE);
        check(strcmp((char *)buffer, "Short") == 0 && intact(buffer, 6),
              "a text of 5 characters stays whole");

        SetAttrs(g, STRINGA_TextVal, (IPTR)NULL, TAG_DONE);
        check(buffer[0] == 0 && intact(buffer, 6), "STRINGA_TextVal NULL empties the buffer");

        SetAttrs(g, STRINGA_LongVal, -1234567, TAG_DONE);
        check(si->LongInt == -1234567 && strcmp((char *)buffer, "-1234") == 0 && intact(buffer, 6),
              "a longer STRINGA_LongVal keeps its first 5 characters");

        SetAttrs(g, STRINGA_LongVal, (IPTR)(LONG)0x80000000, TAG_DONE);
        check(strcmp((char *)buffer, "-2147") == 0 && intact(buffer, 6),
              "the lowest LONG set later, cut to the buffer");

        SetAttrs(g, STRINGA_MaxChars, 30, STRINGA_Buffer, (IPTR)other,
                 STRINGA_UndoBuffer, (IPTR)other, STRINGA_WorkBuffer, (IPTR)other, TAG_DONE);
        check(si->MaxChars == 6 && si->Buffer == (STRPTR)buffer && si->UndoBuffer == (STRPTR)undo &&
              ((struct StringExtend *)si->Extension)->WorkBuffer == (STRPTR)work,
              "MaxChars and the three buffers only at creation");

        SetAttrs(g, STRINGA_TextVal, (IPTR)"Still cut to six", TAG_DONE);
        check(strcmp((char *)buffer, "Still") == 0 && intact(buffer, 6) && other[0] == 0,
              "after them texts are still cut to the buffer of creation");

        DisposeObject((Object *)g);
        check(intact(buffer, 6) && intact(undo, 6) && intact(work, 6),
              "no guard byte touched, also after disposal");
    }

    /* MaxChars 1: room for the NUL alone */
    buffer = guarded(one, 1);
    g = (struct Gadget *)NewObject(NULL, STRGCLASS, STRINGA_MaxChars, 1, STRINGA_Buffer, (IPTR)buffer,
                                   STRINGA_UndoBuffer, (IPTR)guarded(area[1], 6),
                                   STRINGA_WorkBuffer, (IPTR)guarded(area[2], 6),
                                   STRINGA_TextVal, (IPTR)"abc", TAG_DONE);
    check(g != NULL && buffer[0] == 0 && intact(buffer, 1), "MaxChars 1: a text leaves the NUL alone");
    if (g)
    {
        /* Poisoned again, so that only a written NUL passes */
        buffer[0] = GUARDBYTE;
        SetAttrs(g, STRINGA_LongVal, 98765, TAG_DONE);
        check(buffer[0] == 0 && intact(buffer, 1), "MaxChars 1: so does a number");
        DisposeObject((Object *)g);
    }

    /* Numbers at creation */
    buffer = guarded(area[0], 4);
    g = (struct Gadget *)NewObject(NULL, STRGCLASS, STRINGA_MaxChars, 4, STRINGA_Buffer, (IPTR)buffer,
                                   STRINGA_UndoBuffer, (IPTR)guarded(area[1], 4),
                                   STRINGA_WorkBuffer, (IPTR)guarded(area[2], 4),
                                   STRINGA_LongVal, 123456, TAG_DONE);
    check(g && strcmp((char *)buffer, "123") == 0 && intact(buffer, 4),
          "a longer STRINGA_LongVal at creation keeps its first 3 characters");
    if (g)
        DisposeObject((Object *)g);
    g = (struct Gadget *)NewObject(NULL, STRGCLASS, STRINGA_MaxChars, 12,
                                   STRINGA_LongVal, (IPTR)(LONG)0x80000000, TAG_DONE);
    check(g && strcmp((char *)((struct StringInfo *)g->SpecialInfo)->Buffer, "-2147483648") == 0,
          "the lowest LONG at creation, with its digits");
    if (g)
        DisposeObject((Object *)g);

    printf("strgmaxchars: %d checks, %d failed\n", checks, failures);
    return failures ? 5 : 0;
}
