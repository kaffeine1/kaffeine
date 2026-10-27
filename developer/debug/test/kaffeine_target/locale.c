/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    locale.library target test: catalog fallback (OpenCatalogA, GetCatalogStr,
    CloseCatalog), OpenLocale(NULL) guest struct Locale fields and strings,
    character classification and conversion, GetLocaleStr(), StrConvert(),
    StrnCmp(), FormatDate()/ParseDate()/FormatString() through 68k C hooks,
    and CloseLocale().
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/locale.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/locale.h>

#include "m68ktest.h"

struct LocaleBase *LocaleBase;

extern ULONG locale_hook_entry(void);

/* The native hook bridge must enter this 68k stub, which supplies the ordinary
   libnix C stack arguments to h_SubEntry. */
__asm__(
    ".text\n"
    ".even\n"
    ".globl _locale_hook_entry\n"
    "_locale_hook_entry:\n"
    "move.l %a1,-(%sp)\n"
    "move.l %a2,-(%sp)\n"
    "move.l %a0,-(%sp)\n"
    "move.l 12(%a0),%a0\n"
    "jsr (%a0)\n"
    "lea 12(%sp),%sp\n"
    "rts\n"
);

struct DateStream
{
    struct Locale *locale;
    UBYTE bytes[64];
    ULONG count, next;
    BOOL registers_ok;
};

static ULONG put_date_char(struct Hook *hook, struct Locale *object, ULONG character)
{
    struct DateStream *stream = hook->h_Data;

    if (object != stream->locale || character > 255)
        stream->registers_ok = FALSE;
    if (stream->count < sizeof(stream->bytes))
        stream->bytes[stream->count] = character;
    stream->count++;
    return 0;
}

static ULONG get_date_char(struct Hook *hook, struct Locale *object, ULONG message)
{
    struct DateStream *stream = hook->h_Data;

    if (object != stream->locale || message != 0)
        stream->registers_ok = FALSE;
    if (stream->next >= stream->count) return 0;
    return stream->bytes[stream->next++];
}

static void check_date_hooks(struct Locale *loc)
{
    static const char format[] = "%Y-%m-%d %H:%M:%S";
    static const char expected[] = "1978-01-02 02:03:04";
    struct DateStamp date = { 1, 123, 200 }, parsed = { -1, -2, -3 };
    struct DateStream stream = { 0 };
    struct Hook writer = { {0}, locale_hook_entry, (ULONG (*)())put_date_char, &stream };
    struct Hook reader = { {0}, locale_hook_entry, (ULONG (*)())get_date_char, &stream };

    stream.locale = loc;
    stream.registers_ok = TRUE;
    FormatDate(loc, format, &date, &writer);
    check_str("FormatDate() through a 68k C hook", (const char *)stream.bytes, expected);
    check_long("FormatDate() hook counts the terminating zero", stream.count, sizeof(expected));
    check(stream.registers_ok, "FormatDate() hook gets the guest locale in A2 and character in A1");

    check(ParseDate(loc, &parsed, format, &reader) != 0, "ParseDate() reads a 68k C hook");
    check_long("ParseDate() round trip ds_Days", parsed.ds_Days, date.ds_Days);
    check_long("ParseDate() round trip ds_Minute", parsed.ds_Minute, date.ds_Minute);
    check_long("ParseDate() round trip ds_Tick", parsed.ds_Tick, date.ds_Tick);
    check(stream.registers_ok, "ParseDate() hook gets the guest locale in A2 and NULL in A1");
    check_long("ParseDate() consumes the terminating zero", stream.next, sizeof(expected));

    stream.bytes[0] = 'b'; stream.bytes[1] = 'a'; stream.bytes[2] = 'd'; stream.bytes[3] = 0;
    stream.count = 4; stream.next = 0;
    parsed.ds_Days = -1; parsed.ds_Minute = -2; parsed.ds_Tick = -3;
    check_long("ParseDate() rejects a malformed date stream", ParseDate(loc, &parsed, format, &reader), 0);
    check_long("failed ParseDate() preserves ds_Days", parsed.ds_Days, -1);
    check_long("failed ParseDate() preserves ds_Minute", parsed.ds_Minute, -2);
    check_long("failed ParseDate() preserves ds_Tick", parsed.ds_Tick, -3);
}

static void stream_long(UBYTE *stream, ULONG value)
{
    stream[0] = value >> 24;
    stream[1] = value >> 16;
    stream[2] = value >> 8;
    stream[3] = value;
}

static void check_formatstring_hook(struct Locale *loc)
{
    static const char text[] = "Amiga";
    static const UBYTE bcpl[] __attribute__((aligned(4))) = {4, 'B', 'C', 'P', 'L'};
    static const char expected[] = "1234567/-12/Amiga/BCPL/1234567";
    UBYTE data[14] __attribute__((aligned(2)));
    struct DateStream stream = { 0 };
    struct Hook writer = { {0}, locale_hook_entry, (ULONG (*)())put_date_char, &stream };
    APTR next;

    /* A WORD followed by LONGs/pointers, with no native alignment padding. */
    data[0] = 0xFF; data[1] = 0xF4;
    stream_long(data + 2, 1234567);
    stream_long(data + 6, (ULONG)text);
    stream_long(data + 10, ((ULONG)bcpl) >> 2);
    stream.locale = loc;
    stream.registers_ok = TRUE;
    next = FormatString(loc, "%2$ld/%1$d/%3$s/%4$b/%2$lu", data, &writer);
    check_str("FormatString() converts mixed and positional arguments through a C hook",
              (const char *)stream.bytes, expected);
    check_long("FormatString() returns past the highest argument, despite the final repeat",
               (LONG)next, (LONG)(data + sizeof(data)));
    check_long("FormatString() hook counts the terminating zero", stream.count, sizeof(expected));
    check(stream.registers_ok, "FormatString() hook gets the guest locale in A2 and character in A1");

    stream.count = 0;
    next = FormatString(loc, "literal %%", NULL, &writer);
    check_str("FormatString() literal percent needs no stream", (const char *)stream.bytes, "literal %");
    check_long("FormatString() with no arguments returns its original stream", (LONG)next, 0);

    stream.count = 0;
    stream_long(data, 0);
    stream_long(data + 4, 0);
    next = FormatString(loc, "[%s/%b]", data, &writer);
    check_str("FormatString() formats NULL strings as empty", (const char *)stream.bytes, "[/]");
    check_long("FormatString() advances over both NULL string arguments", (LONG)next, (LONG)(data + 8));
}

/* NULL selects the default locale for one call, as in AROS, and the hook then
   gets NULL in A2. A NULL FormatString() template returns the data stream. */
static void check_null_locale_hooks(void)
{
    static const char format[] = "%Y-%m-%d %H:%M:%S";
    static const char expected[] = "1978-01-02 02:03:04";
    struct DateStamp date = { 1, 123, 200 }, parsed = { -1, -2, -3 };
    struct DateStream stream = { 0 };
    struct Hook writer = { {0}, locale_hook_entry, (ULONG (*)())put_date_char, &stream };
    struct Hook reader = { {0}, locale_hook_entry, (ULONG (*)())get_date_char, &stream };
    UBYTE data[4] __attribute__((aligned(2)));
    APTR next;

    stream.locale = NULL;
    stream.registers_ok = TRUE;
    FormatDate(NULL, format, &date, &writer);
    check_str("FormatDate(NULL) formats with the default locale", (const char *)stream.bytes, expected);
    check(ParseDate(NULL, &parsed, format, &reader) != 0 && parsed.ds_Days == date.ds_Days &&
          parsed.ds_Minute == date.ds_Minute && parsed.ds_Tick == date.ds_Tick,
          "ParseDate(NULL) reads the date back with the default locale");

    stream.count = 0;
    stream_long(data, 1234567);
    next = FormatString(NULL, "%ld", data, &writer);
    check_str("FormatString(NULL) formats with the default locale", (const char *)stream.bytes, "1234567");
    check_long("FormatString(NULL) returns past its argument", (LONG)next, (LONG)(data + 4));
    check(stream.registers_ok, "a NULL locale reaches each 68k hook as NULL in A2");

    stream.count = 0;
    next = FormatString(NULL, NULL, data, &writer);
    check(next == data && stream.count == 0,
          "a NULL FormatString() template returns the stream without output");
}

int main(void)
{
    static const char builtin_msg[] = "Built-in fallback message";
    struct Catalog *cat;
    STRPTR cat_str;
    struct Locale *loc;
    STRPTR day1, day1_again;
    UBYTE conv_buf[8];
    ULONG conv_len;

    LocaleBase = (struct LocaleBase *)OpenLibrary((CONST_STRPTR)"locale.library", 38);
    if (!check(LocaleBase != NULL, "OpenLibrary() of locale.library"))
        return test_done("locale");

    /* Catalog fallback: OpenCatalogA returns NULL, GetCatalogStr returns the
       caller's defaultString pointer unchanged, CloseCatalog is a no-op. */
    cat = OpenCatalogA(NULL, (STRPTR)"nonexistent.catalog", NULL);
    check_long("OpenCatalogA() returns NULL", (LONG)cat, 0);

    cat_str = GetCatalogStr(cat, 1, (STRPTR)builtin_msg);
    check_long("GetCatalogStr() returns defaultString pointer unchanged",
               (LONG)cat_str, (LONG)builtin_msg);
    check_str("GetCatalogStr() defaultString is readable",
              (const char *)cat_str, builtin_msg);

    CloseCatalog(cat);
    CloseCatalog(NULL);

    /* OpenLocale(NULL) opens the default locale and builds a guest copy. */
    loc = OpenLocale(NULL);
    if (!check(loc != NULL, "OpenLocale(NULL) succeeds"))
    {
        CloseLibrary((struct Library *)LocaleBase);
        return test_done("locale");
    }

    check(loc->loc_LocaleName != NULL && loc->loc_LocaleName[0] != '\0',
          "loc_LocaleName is non-empty");
    check(loc->loc_LanguageName != NULL && loc->loc_LanguageName[0] != '\0',
          "loc_LanguageName is non-empty");
    check(loc->loc_DateTimeFormat != NULL && loc->loc_DateTimeFormat[0] != '\0',
          "loc_DateTimeFormat is non-empty");
    check(loc->loc_DateFormat != NULL && loc->loc_DateFormat[0] != '\0',
          "loc_DateFormat is non-empty");
    check(loc->loc_TimeFormat != NULL && loc->loc_TimeFormat[0] != '\0',
          "loc_TimeFormat is non-empty");
    check(loc->loc_ShortDateTimeFormat != NULL &&
          loc->loc_ShortDateTimeFormat[0] != '\0',
          "loc_ShortDateTimeFormat is non-empty");
    check(loc->loc_ShortDateFormat != NULL && loc->loc_ShortDateFormat[0] != '\0',
          "loc_ShortDateFormat is non-empty");
    check(loc->loc_ShortTimeFormat != NULL && loc->loc_ShortTimeFormat[0] != '\0',
          "loc_ShortTimeFormat is non-empty");
    check(loc->loc_DecimalPoint != NULL && loc->loc_DecimalPoint[0] != '\0',
          "loc_DecimalPoint is non-empty");
    check(loc->loc_GroupSeparator != NULL, "loc_GroupSeparator is non-NULL");
    check(loc->loc_FracGroupSeparator != NULL, "loc_FracGroupSeparator is non-NULL");
    check(loc->loc_Grouping != NULL, "loc_Grouping is non-NULL");
    check(loc->loc_FracGrouping != NULL, "loc_FracGrouping is non-NULL");
    check(loc->loc_MonDecimalPoint != NULL, "loc_MonDecimalPoint is non-NULL");
    check(loc->loc_MonGroupSeparator != NULL, "loc_MonGroupSeparator is non-NULL");
    check(loc->loc_MonFracGroupSeparator != NULL,
          "loc_MonFracGroupSeparator is non-NULL");
    check(loc->loc_MonGrouping != NULL, "loc_MonGrouping is non-NULL");
    check(loc->loc_MonFracGrouping != NULL, "loc_MonFracGrouping is non-NULL");
    check(loc->loc_MonCS != NULL, "loc_MonCS is non-NULL");
    check(loc->loc_MonSmallCS != NULL, "loc_MonSmallCS is non-NULL");
    check(loc->loc_MonIntCS != NULL, "loc_MonIntCS is non-NULL");
    check(loc->loc_MonPositiveSign != NULL, "loc_MonPositiveSign is non-NULL");
    check(loc->loc_MonNegativeSign != NULL, "loc_MonNegativeSign is non-NULL");

    /* Character classification and conversion. */
    check(IsAlpha(loc, 'A') != 0, "IsAlpha(loc, 'A') is true");
    check_long("IsAlpha(loc, '5') is false", IsAlpha(loc, '5'), 0);
    check_long("IsAlpha(NULL, 'A') is false", IsAlpha(NULL, 'A'), 0);

    check(IsDigit(loc, '5') != 0, "IsDigit(loc, '5') is true");
    check_long("IsDigit(loc, 'A') is false", IsDigit(loc, 'A'), 0);
    check_long("IsDigit(NULL, '5') is false", IsDigit(NULL, '5'), 0);

    check(IsAlNum(loc, '9') != 0, "IsAlNum(loc, '9') is true");
    check(IsSpace(loc, ' ') != 0, "IsSpace(loc, ' ') is true");
    check(IsUpper(loc, 'Z') != 0, "IsUpper(loc, 'Z') is true");
    check(IsLower(loc, 'z') != 0, "IsLower(loc, 'z') is true");
    check(IsXDigit(loc, 'f') != 0, "IsXDigit(loc, 'f') is true");

    check_long("ConvToUpper(loc, 'a')", (LONG)ConvToUpper(loc, 'a'), 'A');
    check_long("ConvToLower(loc, 'A')", (LONG)ConvToLower(loc, 'A'), 'a');
    check_long("ConvToUpper(NULL, 'a') returns character unchanged",
               (LONG)ConvToUpper(NULL, 'a'), 'a');
    check_long("ConvToLower(NULL, 'A') returns character unchanged",
               (LONG)ConvToLower(NULL, 'A'), 'A');

    /* GetLocaleStr() copies into guest memory once per locale and stringNum. */
    day1 = GetLocaleStr(loc, DAY_1);
    if (check(day1 != NULL && day1[0] != '\0', "GetLocaleStr(loc, DAY_1) is non-empty"))
    {
        day1_again = GetLocaleStr(loc, DAY_1);
        check_long("GetLocaleStr(loc, DAY_1) returns the same guest copy",
                   (LONG)day1_again, (LONG)day1);
    }
    check_long("GetLocaleStr(loc, 71) out of range returns NULL",
               (LONG)GetLocaleStr(loc, 71), 0);
    check_long("GetLocaleStr(NULL, DAY_1) returns NULL",
               (LONG)GetLocaleStr(NULL, DAY_1), 0);

    /* StrConvert() transforms into a guest buffer and NUL-terminates it. */
    conv_buf[0] = 0x5A;
    conv_buf[1] = 0x5A;
    conv_len = StrConvert(loc, (STRPTR)"a", conv_buf, 2, SC_ASCII);
    check_long("StrConvert(loc, \"a\", buf, 2, SC_ASCII) returns 1",
               (LONG)conv_len, 1);
    check(conv_buf[0] != 0 && conv_buf[1] == 0,
          "StrConvert() writes transformed byte and NUL terminator");

    conv_buf[0] = 0x7F;
    check_long("StrConvert() with bufferSize 0 returns 0",
               (LONG)StrConvert(loc, (STRPTR)"a", conv_buf, 0, SC_ASCII), 0);
    check_long("StrConvert() with bufferSize 0 writes nothing",
               (LONG)conv_buf[0], 0x7F);

    /* StrnCmp() compares strings under the locale's collation rules. */
    check_long("StrnCmp() equal case-insensitive strings",
               StrnCmp(loc, (STRPTR)"Amiga", (STRPTR)"amiga", -1, SC_ASCII), 0);
    check_long("StrnCmp() prefix length limit",
               StrnCmp(loc, (STRPTR)"Abx", (STRPTR)"Aby", 2, SC_ASCII), 0);
    check(StrnCmp(loc, (STRPTR)"Alpha", (STRPTR)"Beta", -1, SC_ASCII) < 0,
          "StrnCmp(\"Alpha\", \"Beta\") < 0");
    check(StrnCmp(loc, (STRPTR)"Beta", (STRPTR)"Alpha", -1, SC_ASCII) > 0,
          "StrnCmp(\"Beta\", \"Alpha\") > 0");

    check_date_hooks(loc);
    check_formatstring_hook(loc);
    check_null_locale_hooks();

    /* Guest FreeMem() must not be able to free the open guest Locale block. */
    FreeMem(loc, sizeof(struct Locale));
    check_long("ConvToUpper() still works after refused FreeMem(loc)",
               (LONG)ConvToUpper(loc, 'b'), 'B');

    CloseLocale(loc);
    CloseLocale(loc);
    CloseLocale(NULL);
    check_long("IsAlpha() on closed locale returns 0", IsAlpha(loc, 'A'), 0);

    CloseLibrary((struct Library *)LocaleBase);
    return test_done("locale");
}
