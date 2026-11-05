/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   keymap.library MapANSI with the system default keymap, called the way
   amigentd 1.1.1 types text. Whatever the keymap, the pairs of every
   conversion must type their character again through MapRawKey, with the
   pairs before the last as the dead-key history amigentd keeps. Then the
   documented results: several characters, a full buffer with the pair
   written before it, zero counts and lengths and an odd buffer. Argument
   "dead" also requires a dead-key prefix for e acute, as the AROS default
   keymap gives. */
#include <exec/types.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/keymap.h>
#include <string.h>
#include "m68ktest.h"

struct Library *KeymapBase;

/* amigentd's type_char(): both buffers as words, eight pairs of room */
static LONG type_char(UWORD ch, UWORD *buf)
{
    UWORD in = ch << 8;
    return MapANSI((STRPTR)&in, 1, (STRPTR)buf, 8, NULL);
}

/* The character that the last of n pairs types, the pairs before it
   being the last two keys down; -1 unless MapRawKey gives one byte */
static LONG typed(const UBYTE *keys, LONG n)
{
    struct InputEvent ev;
    UBYTE out[4];

    memset(&ev, 0, sizeof(ev));
    ev.ie_Class = IECLASS_RAWKEY;
    ev.ie_Code = keys[2 * (n - 1)];
    ev.ie_Qualifier = keys[2 * (n - 1) + 1];
    if (n > 1)
    {
        ev.ie_Prev1DownCode = keys[2 * (n - 2)];
        ev.ie_Prev1DownQual = keys[2 * (n - 2) + 1];
    }
    if (n > 2)
    {
        ev.ie_Prev2DownCode = keys[2 * (n - 3)];
        ev.ie_Prev2DownQual = keys[2 * (n - 3) + 1];
    }
    return MapRawKey(&ev, (STRPTR)out, sizeof(out), NULL) == 1 ? out[0] : -1;
}

static BOOL untouched(const UBYTE *bytes, LONG from, LONG to)
{
    LONG i;
    for (i = from; i < to; i++)
        if (bytes[i] != 0xA5)
            return FALSE;
    return TRUE;
}

/* One character through type_char(): 0 when the keymap lacks it, -1 when
   its pairs do not type it again, else the number of pairs */
static LONG round_trip(UWORD ch)
{
    UWORD buf[8];
    LONG n;

    memset(buf, 0xA5, sizeof(buf));
    n = type_char(ch, buf);
    if (n == 0)
        return 0;
    if (n < 1 || n > 3 || !untouched((UBYTE *)buf, 2 * n, sizeof(buf)) || typed((UBYTE *)buf, n) != ch)
        return -1;
    return n;
}

static const struct { UBYTE ch; const char *what; } required[] =
{
    { 'a', "a lower-case letter types itself again" },
    { 'Z', "an upper-case letter types itself again" },
    { '7', "a digit types itself again" },
    { ' ', "a space types itself again" },
    { '.', "a full stop types itself again" },
};
static const struct { UBYTE ch; const char *name; } latin1[] =
{
    { 0xE9, "e acute" }, { 0xFC, "u diaeresis" }, { 0xE0, "a grave" },
    { 0xF1, "n tilde" }, { 0xDF, "sharp s" }, { 0xA3, "pound sign" },
};

int main(int argc, char **argv)
{
    UWORD words[9];
    UBYTE *bytes = (UBYTE *)words, keys[16], a_pair[2];
    LONG i, n, dead = 0;
    BOOL ok;

    KeymapBase = OpenLibrary((CONST_STRPTR)"keymap.library", 37);
    if (!check(KeymapBase != NULL, "open keymap.library"))
        return test_done("keymap_mapansi");

    for (i = 0; i < (LONG)(sizeof(required) / sizeof(required[0])); i++)
        check(round_trip(required[i].ch) == 1, required[i].what);

    /* Latin-1 characters may be absent, single keys or dead-key sequences */
    ok = TRUE;
    for (i = 0; i < (LONG)(sizeof(latin1) / sizeof(latin1[0])); i++)
    {
        n = round_trip(latin1[i].ch);
        PutStr((CONST_STRPTR)latin1[i].name);
        PutStr((CONST_STRPTR)(n == 0 ? ": not in this keymap\n" : n < 0 ? ": wrong pairs\n" :
                              n == 1 ? ": one key\n" : ": dead-key sequence\n"));
        if (n < 0)
            ok = FALSE;
        if (latin1[i].ch == 0xE9 && n > 1)
            dead = n;
    }
    check(ok, "every Latin-1 character the keymap has types itself again, dead keys included");
    if (argc > 1 && !strcmp(argv[1], "dead"))
        check(dead > 1, "e acute comes with a dead-key prefix");

    /* Several characters in one call: one pair each */
    memset(keys, 0xA5, sizeof(keys));
    n = MapANSI((STRPTR)"Hi 7", 4, (STRPTR)keys, 8, NULL);
    ok = n == 4 && untouched(keys, 8, sizeof(keys));
    for (i = 0; ok && i < 4; i++)
        ok = typed(keys + 2 * i, 1) == (UBYTE)"Hi 7"[i];
    check(ok, "four characters give four pairs that type them again");

    /* A full buffer keeps the pair written before it */
    memset(keys, 0xA5, sizeof(keys));
    check(MapANSI((STRPTR)"a", 1, (STRPTR)a_pair, 1, NULL) == 1, "one character fills one pair");
    check(MapANSI((STRPTR)"ab", 2, (STRPTR)keys, 1, NULL) == -1 && keys[0] == a_pair[0] &&
          keys[1] == a_pair[1] && untouched(keys, 2, sizeof(keys)),
          "a full buffer returns -1 after writing the pair that fits");

    /* Nothing to convert, or no room */
    memset(keys, 0xA5, sizeof(keys));
    check(MapANSI((STRPTR)"a", 0, (STRPTR)keys, 8, NULL) == 0 && untouched(keys, 0, sizeof(keys)),
          "a zero count returns 0 and writes nothing");
    check(MapANSI((STRPTR)"a", 1, (STRPTR)keys, 0, NULL) == -1 && untouched(keys, 0, sizeof(keys)),
          "a zero length returns -1 and writes nothing");

    /* Pairs are bytes: an odd buffer works */
    memset(words, 0xA5, sizeof(words));
    check(MapANSI((STRPTR)"a", 1, (STRPTR)bytes + 1, 1, NULL) == 1 && bytes[0] == 0xA5 &&
          bytes[1] == a_pair[0] && bytes[2] == a_pair[1] && untouched(bytes, 3, sizeof(words)),
          "an odd buffer gets the same pair and nothing else");

    CloseLibrary(KeymapBase);
    return test_done("keymap_mapansi");
}
