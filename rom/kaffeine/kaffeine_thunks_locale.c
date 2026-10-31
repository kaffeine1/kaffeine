/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    kaffeine.library: locale.library manual thunks (catalog fallback and
    guest Locale copies)
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <libraries/locale.h>
#include <proto/exec.h>
#include <proto/locale.h>
#include <aros/asmcall.h>
#include <stdint.h>
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_thunks.h"
#include "kaffeine_thunks_locale.h"
#include "kaffeine_hook.h"

/* Return a host pointer to a NUL-terminated guest string, or NULL if the
   address is 0, outside guest memory, or not terminated before mem_size. */
static const char *guest_string(struct KaffeineContext *ctx, ULONG addr)
{
    ULONG rem;
    const char *s, *end;

    if (!addr || addr >= ctx->mem_size || ctx->mem_size - addr < 1) return NULL;
    rem = ctx->mem_size - addr;
    s = (const char *)(ctx->mem + addr);
    end = (const char *)memchr(s, '\0', rem);
    return end ? s : NULL;
}

static int find_locale_slot(struct KaffeineContext *ctx, ULONG guest_addr)
{
    if (!guest_addr) return -1;
    for (int i = 0; i < KAFFEINE_MAX_LOCALES; i++)
    {
        if (ctx->locale_map[i].guest_addr == guest_addr &&
            ctx->locale_map[i].native_locale != NULL &&
            !ctx->locale_map[i].close_pending)
            return i;
    }
    return -1;
}

static int find_free_locale_slot(struct KaffeineContext *ctx)
{
    for (int i = 0; i < KAFFEINE_MAX_LOCALES; i++)
    {
        if (ctx->locale_map[i].guest_addr == 0 &&
            ctx->locale_map[i].native_locale == NULL)
            return i;
    }
    return -1;
}

static BOOL add_str_size(const void *p, ULONG *total)
{
    ULONG len;

    if (!p) return TRUE;
    len = (ULONG)strlen((const char *)p) + 1;
    if (len > (ULONG)~0UL - *total) return FALSE;
    *total += len;
    return TRUE;
}

/* AROS keeps the four grouping vectors as fixed UBYTE[10] arrays
   (workbench/libs/locale/locale_intern.h), which a country file can fill
   without a zero: copy them with that bound, plus a zero after them. */
#define LOCALE_GROUPING_SIZE 10

static BOOL add_vec_size(const void *p, ULONG *total)
{
    if (!p) return TRUE;
    if (LOCALE_GROUPING_SIZE + 1 > (ULONG)~0UL - *total) return FALSE;
    *total += LOCALE_GROUPING_SIZE + 1;
    return TRUE;
}

static void write_vec_copy(struct KaffeineContext *ctx, ULONG struct_base,
                           ULONG field_offset, const void *src, ULONG *cursor)
{
    ULONG dst = *cursor;

    if (!src)
    {
        m68k_write32(ctx, struct_base + field_offset, 0);
        return;
    }
    memcpy(ctx->mem + dst, src, LOCALE_GROUPING_SIZE);
    ctx->mem[dst + LOCALE_GROUPING_SIZE] = 0;
    m68k_write32(ctx, struct_base + field_offset, dst);
    *cursor += LOCALE_GROUPING_SIZE + 1;
}

static void write_str_copy(struct KaffeineContext *ctx, ULONG struct_base,
                           ULONG field_offset, const void *src, ULONG *cursor)
{
    ULONG len, dst;

    if (!src)
    {
        m68k_write32(ctx, struct_base + field_offset, 0);
        return;
    }
    len = (ULONG)strlen((const char *)src) + 1;
    dst = *cursor;
    memcpy(ctx->mem + dst, src, len);
    m68k_write32(ctx, struct_base + field_offset, dst);
    *cursor += len;
}

static ULONG build_guest_locale(struct KaffeineContext *ctx,
                                const struct Locale *loc)
{
    ULONG total = M68K_SIZEOF_LOCALE;
    ULONG base, cursor;

    if (!add_str_size(loc->loc_LocaleName, &total) ||
        !add_str_size(loc->loc_LanguageName, &total))
        return 0;

    for (int i = 0; i < M68K_LOC_PREFLANGUAGES_COUNT; i++)
    {
        if (!add_str_size(loc->loc_PrefLanguages[i], &total))
            return 0;
    }

    if (!add_str_size(loc->loc_DateTimeFormat, &total) ||
        !add_str_size(loc->loc_DateFormat, &total) ||
        !add_str_size(loc->loc_TimeFormat, &total) ||
        !add_str_size(loc->loc_ShortDateTimeFormat, &total) ||
        !add_str_size(loc->loc_ShortDateFormat, &total) ||
        !add_str_size(loc->loc_ShortTimeFormat, &total) ||
        !add_str_size(loc->loc_DecimalPoint, &total) ||
        !add_str_size(loc->loc_GroupSeparator, &total) ||
        !add_str_size(loc->loc_FracGroupSeparator, &total) ||
        !add_vec_size(loc->loc_Grouping, &total) ||
        !add_vec_size(loc->loc_FracGrouping, &total) ||
        !add_str_size(loc->loc_MonDecimalPoint, &total) ||
        !add_str_size(loc->loc_MonGroupSeparator, &total) ||
        !add_str_size(loc->loc_MonFracGroupSeparator, &total) ||
        !add_vec_size(loc->loc_MonGrouping, &total) ||
        !add_vec_size(loc->loc_MonFracGrouping, &total) ||
        !add_str_size(loc->loc_MonCS, &total) ||
        !add_str_size(loc->loc_MonSmallCS, &total) ||
        !add_str_size(loc->loc_MonIntCS, &total) ||
        !add_str_size(loc->loc_MonPositiveSign, &total) ||
        !add_str_size(loc->loc_MonNegativeSign, &total))
        return 0;

    base = Kaffeine_LocaleAlloc(ctx, total);
    if (!base) return 0;

    cursor = base + M68K_SIZEOF_LOCALE;

    write_str_copy(ctx, base, M68K_LOC_LOCALENAME, loc->loc_LocaleName, &cursor);
    write_str_copy(ctx, base, M68K_LOC_LANGUAGENAME, loc->loc_LanguageName, &cursor);
    for (int i = 0; i < M68K_LOC_PREFLANGUAGES_COUNT; i++)
    {
        write_str_copy(ctx, base, M68K_LOC_PREFLANGUAGES + (ULONG)(i * 4),
                       loc->loc_PrefLanguages[i], &cursor);
    }

    m68k_write32(ctx, base + M68K_LOC_FLAGS, loc->loc_Flags);
    m68k_write32(ctx, base + M68K_LOC_CODESET, loc->loc_CodeSet);
    m68k_write32(ctx, base + M68K_LOC_COUNTRYCODE, loc->loc_CountryCode);
    m68k_write32(ctx, base + M68K_LOC_TELEPHONECODE, loc->loc_TelephoneCode);
    m68k_write32(ctx, base + M68K_LOC_GMTOFFSET, (ULONG)loc->loc_GMTOffset);
    ctx->mem[base + M68K_LOC_MEASURINGSYSTEM] = loc->loc_MeasuringSystem;
    ctx->mem[base + M68K_LOC_CALENDARTYPE] = loc->loc_CalendarType;
    ctx->mem[base + M68K_LOC_RESERVED0] = loc->loc_Reserved0[0];
    ctx->mem[base + M68K_LOC_RESERVED0 + 1] = loc->loc_Reserved0[1];

    write_str_copy(ctx, base, M68K_LOC_DATETIMEFORMAT, loc->loc_DateTimeFormat, &cursor);
    write_str_copy(ctx, base, M68K_LOC_DATEFORMAT, loc->loc_DateFormat, &cursor);
    write_str_copy(ctx, base, M68K_LOC_TIMEFORMAT, loc->loc_TimeFormat, &cursor);
    write_str_copy(ctx, base, M68K_LOC_SHORTDATETIMEFORMAT, loc->loc_ShortDateTimeFormat, &cursor);
    write_str_copy(ctx, base, M68K_LOC_SHORTDATEFORMAT, loc->loc_ShortDateFormat, &cursor);
    write_str_copy(ctx, base, M68K_LOC_SHORTTIMEFORMAT, loc->loc_ShortTimeFormat, &cursor);
    write_str_copy(ctx, base, M68K_LOC_DECIMALPOINT, loc->loc_DecimalPoint, &cursor);
    write_str_copy(ctx, base, M68K_LOC_GROUPSEPARATOR, loc->loc_GroupSeparator, &cursor);
    write_str_copy(ctx, base, M68K_LOC_FRACGROUPSEPARATOR, loc->loc_FracGroupSeparator, &cursor);
    write_vec_copy(ctx, base, M68K_LOC_GROUPING, loc->loc_Grouping, &cursor);
    write_vec_copy(ctx, base, M68K_LOC_FRACGROUPING, loc->loc_FracGrouping, &cursor);
    write_str_copy(ctx, base, M68K_LOC_MONDECIMALPOINT, loc->loc_MonDecimalPoint, &cursor);
    write_str_copy(ctx, base, M68K_LOC_MONGROUPSEPARATOR, loc->loc_MonGroupSeparator, &cursor);
    write_str_copy(ctx, base, M68K_LOC_MONFRACGROUPSEPARATOR, loc->loc_MonFracGroupSeparator, &cursor);
    write_vec_copy(ctx, base, M68K_LOC_MONGROUPING, loc->loc_MonGrouping, &cursor);
    write_vec_copy(ctx, base, M68K_LOC_MONFRACGROUPING, loc->loc_MonFracGrouping, &cursor);

    ctx->mem[base + M68K_LOC_MONFRACDIGITS] = loc->loc_MonFracDigits;
    ctx->mem[base + M68K_LOC_MONINTFRACDIGITS] = loc->loc_MonIntFracDigits;
    ctx->mem[base + M68K_LOC_RESERVED1] = loc->loc_Reserved1[0];
    ctx->mem[base + M68K_LOC_RESERVED1 + 1] = loc->loc_Reserved1[1];

    write_str_copy(ctx, base, M68K_LOC_MONCS, loc->loc_MonCS, &cursor);
    write_str_copy(ctx, base, M68K_LOC_MONSMALLCS, loc->loc_MonSmallCS, &cursor);
    write_str_copy(ctx, base, M68K_LOC_MONINTCS, loc->loc_MonIntCS, &cursor);
    write_str_copy(ctx, base, M68K_LOC_MONPOSITIVESIGN, loc->loc_MonPositiveSign, &cursor);

    ctx->mem[base + M68K_LOC_MONPOSITIVESPACESEP] = loc->loc_MonPositiveSpaceSep;
    ctx->mem[base + M68K_LOC_MONPOSITIVESIGNPOS] = loc->loc_MonPositiveSignPos;
    ctx->mem[base + M68K_LOC_MONPOSITIVECSPOS] = loc->loc_MonPositiveCSPos;
    ctx->mem[base + M68K_LOC_RESERVED2] = loc->loc_Reserved2;

    write_str_copy(ctx, base, M68K_LOC_MONNEGATIVESIGN, loc->loc_MonNegativeSign, &cursor);

    ctx->mem[base + M68K_LOC_MONNEGATIVESPACESEP] = loc->loc_MonNegativeSpaceSep;
    ctx->mem[base + M68K_LOC_MONNEGATIVESIGNPOS] = loc->loc_MonNegativeSignPos;
    ctx->mem[base + M68K_LOC_MONNEGATIVECSPOS] = loc->loc_MonNegativeCSPos;
    ctx->mem[base + M68K_LOC_RESERVED3] = loc->loc_Reserved3;

    return base;
}

static void release_locale_slot(struct KaffeineContext *ctx, int slot)
{
    struct Locale *native_loc = (struct Locale *)ctx->locale_map[slot].native_locale;
    ULONG guest_addr = ctx->locale_map[slot].guest_addr;

    for (int i = 0; i < KAFFEINE_LOCALE_MAXSTRMSG; i++)
    {
        if (ctx->locale_map[slot].str_addrs[i])
        {
            Kaffeine_LocaleFree(ctx, ctx->locale_map[slot].str_addrs[i]);
            ctx->locale_map[slot].str_addrs[i] = 0;
        }
    }
    if (guest_addr)
        Kaffeine_LocaleFree(ctx, guest_addr);
    ctx->locale_map[slot].guest_addr = 0;
    ctx->locale_map[slot].native_locale = NULL;
    ctx->locale_map[slot].active_calls = 0;
    ctx->locale_map[slot].close_pending = FALSE;
    if (native_loc)
        CloseLocale(native_loc);
}

void Kaffeine_LocaleCleanup(struct KaffeineContext *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < KAFFEINE_MAX_LOCALES; i++)
    {
        if (ctx->locale_map[i].guest_addr || ctx->locale_map[i].native_locale)
            release_locale_slot(ctx, i);
    }
}

/* -36: CloseCatalog(A0=catalog) */
static IPTR kaffeine_locale_CloseCatalog(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    return THUNK_D(0);
}

/* -42: CloseLocale(A0=locale) */
static IPTR kaffeine_locale_CloseLocale(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot >= 0)
    {
        if (ctx->locale_map[slot].active_calls)
            ctx->locale_map[slot].close_pending = TRUE;
        else
            release_locale_slot(ctx, slot);
    }
    return THUNK_D(0);
}

/* -48: ConvToLower(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_ConvToLower(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return (IPTR)character;
    return (IPTR)ConvToLower((struct Locale *)ctx->locale_map[slot].native_locale,
                             character);
}

/* -54: ConvToUpper(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_ConvToUpper(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return (IPTR)character;
    return (IPTR)ConvToUpper((struct Locale *)ctx->locale_map[slot].native_locale,
                             character);
}

struct LocaleHookData
{
    struct Locale *native_locale;
    ULONG guest_locale;
    BOOL input;
};

static BOOL locale_hook_args(void *data, APTR object, IPTR message,
                             ULONG *guest_object, ULONG *guest_message)
{
    const struct LocaleHookData *args = data;

    if (object != args->native_locale || (args->input && message != 0))
        return FALSE;
    *guest_object = args->guest_locale;
    /* Output hooks pass a native char, which may be sign-extended. */
    *guest_message = args->input ? 0 : (ULONG)(UBYTE)message;
    return TRUE;
}

static BOOL guest_date_range(struct KaffeineContext *ctx, ULONG addr)
{
    if (!addr || (addr & 1) || addr >= ctx->mem_size || ctx->mem_size - addr < 12)
        return FALSE;
    return TRUE;
}

/* Native date functions advance past a conversion code. Reject a dangling
   '%' and keep a private template so a guest callback cannot remove its NUL. */
static char *copy_date_template(struct KaffeineContext *ctx, ULONG addr, ULONG *size)
{
    const char *src = guest_string(ctx, addr);
    const char *p;
    char *copy;

    if (!src) return NULL;
    for (p = src; *p; p++)
    {
        if (*p == '%' && !*++p) return NULL;
    }
    *size = (ULONG)strlen(src) + 1;
    copy = AllocMem(*size, MEMF_ANY);
    if (copy) memcpy(copy, src, *size);
    return copy;
}

static void finish_locale_call(struct KaffeineContext *ctx, int slot)
{
    ctx->locale_map[slot].active_calls--;
    /* After a stop, leave guest blocks to the ordinary context cleanup. */
    if (ctx->running && !ctx->locale_map[slot].active_calls &&
        ctx->locale_map[slot].close_pending)
        release_locale_slot(ctx, slot);
}

/* FormatDate(), ParseDate() and FormatString() take NULL for the default
   locale, as in AROS: the native default locale is opened for this one call.
   It has no guest copy, so the 68k hook gets NULL as its object. */
static struct Locale *enter_locale_call(struct KaffeineContext *ctx, int slot)
{
    if (slot < 0) return OpenLocale(NULL);
    ctx->locale_map[slot].active_calls++;
    return ctx->locale_map[slot].native_locale;
}

static void leave_locale_call(struct KaffeineContext *ctx, int slot, struct Locale *native)
{
    if (slot < 0) CloseLocale(native);
    else finish_locale_call(ctx, slot);
}

/* -60: FormatDate(A0=locale, A1=template, A2=date, A3=putCharFunc) */
static IPTR kaffeine_locale_FormatDate(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale = THUNK_A(0), date_addr = THUNK_A(2), size;
    int slot = find_locale_slot(ctx, locale);
    struct DateStamp date;
    struct KaffeineNativeHook hook;
    struct LocaleHookData args;
    char *format;

    if ((locale && slot < 0) || !guest_date_range(ctx, date_addr)) return THUNK_D(0);
    args.native_locale = NULL;
    args.guest_locale = locale;
    args.input = FALSE;
    if (!Kaffeine_InitNativeHook(&hook, ctx, cpu, THUNK_A(3), locale_hook_args, &args))
        return THUNK_D(0);
    format = copy_date_template(ctx, THUNK_A(1), &size);
    if (!format) return THUNK_D(0);
    date.ds_Days = (LONG)m68k_read32(ctx, date_addr);
    date.ds_Minute = (LONG)m68k_read32(ctx, date_addr + 4);
    date.ds_Tick = (LONG)m68k_read32(ctx, date_addr + 8);

    args.native_locale = enter_locale_call(ctx, slot);
    if (args.native_locale)
    {
        FormatDate(args.native_locale, (CONST_STRPTR)format, &date, &hook.hook);
        leave_locale_call(ctx, slot, args.native_locale);
    }
    FreeMem(format, size);
    if (hook.failed || !ctx->running) return (IPTR)ctx->exit_code;
    return THUNK_D(0);
}

/* FormatString uses the rules in workbench/libs/locale/formatstring.c, not
   RawDoFmt's sequential parser. Render before calling the guest: neither
   native packed loads nor callbacks may read mutable guest arguments. */
#define LOCALE_FMT_MAX 0x7FFFFFFFu

struct LocaleFormatField
{
    ULONG position, size, kind, width, limit;
    UBYTE code, fill;
    BOOL left;
};

struct LocaleFormatArg
{
    ULONG size, kind, length;
    uint64_t value;
    const UBYTE *body;
};

static BOOL locale_format_decimal(const char **cursor, ULONG *value)
{
    const char *p = *cursor;
    *value = 0;
    while (*p >= '0' && *p <= '9')
    {
        ULONG digit = (ULONG)(*p++ - '0');
        if (*value > (LOCALE_FMT_MAX - digit) / 10) return FALSE;
        *value = *value * 10 + digit;
    }
    *cursor = p;
    return TRUE;
}

/* cursor starts just after '%'. Explicit positions select an argument;
   implicit positions count consuming conversions, as InternalFormatString
   does. Unknown codes and %% do not consume an argument. */
static BOOL locale_format_field(const char **cursor, ULONG *ordinal,
                                struct LocaleFormatField *field)
{
    const char *p = *cursor, *start = p;
    ULONG position = 0;

    memset(field, 0, sizeof(*field));
    field->size = 2;
    field->fill = ' ';
    field->limit = LOCALE_FMT_MAX;
    if (*p == '%')
    {
        field->code = '%';
        *cursor = p + 1;
        return TRUE;
    }
    if (!locale_format_decimal(&p, &position)) return FALSE;
    if (*p == '$')
    {
        if (!position) return FALSE;
        p++;
    }
    else { position = *ordinal + 1; p = start; }
    if (*p == '-') { field->left = TRUE; p++; }
    if (*p == '0') { field->fill = '0'; p++; }
    if (!locale_format_decimal(&p, &field->width)) return FALSE;
    if (*p == '.')
    {
        p++;
        if (*p >= '0' && *p <= '9' &&
            !locale_format_decimal(&p, &field->limit)) return FALSE;
    }
    if (*p == 'l' || *p == 'i')
    {
        field->size = 4;
        if (*p++ == 'l' && *p == 'l') { field->size = 8; p++; }
    }
    else if (*p == 'L') { field->size = 8; p++; }
    if (!*p) return FALSE;
    field->code = (UBYTE)*p++;
    switch (field->code)
    {
    case 's': case 'b':
        field->size = 4;
        field->kind = field->code;
        break;
    case 'p': case 'P':
        if (field->size < 4) field->size = 4;
        field->width = 8;
        field->fill = '0';
        break;
    case 'd': case 'D': case 'u': case 'U': case 'x': case 'X': case 'c':
        break;
    default:
        field->width = 1;
        *cursor = p;
        return TRUE;
    }
    field->position = position;
    (*ordinal)++;
    *cursor = p;
    return TRUE;
}

static BOOL locale_format_args(struct KaffeineContext *ctx, const char *format,
                               ULONG stream, struct LocaleFormatArg *args,
                               ULONG count, ULONG *next)
{
    const char *p = format;
    ULONG ordinal = 0, i, addr = stream;
    struct LocaleFormatField field;

    while (*p)
    {
        if (*p++ != '%') continue;
        if (!locale_format_field(&p, &ordinal, &field)) return FALSE;
        if (!field.position) continue;
        if (field.position > count) return FALSE;
        i = field.position - 1;
        if (args[i].size && (args[i].size != field.size || args[i].kind != field.kind))
            return FALSE;
        args[i].size = field.size;
        args[i].kind = field.kind;
    }
    for (i = 0; i < count; i++)
    {
        struct LocaleFormatArg *arg = &args[i];
        ULONG value;
        /* Missing positions have unknown widths. Guessing would let a later
           conversion read the wrong bytes or mistake a number for a string. */
        if (!arg->size || !addr || (addr & 1) || addr >= ctx->mem_size ||
            ctx->mem_size - addr < arg->size) return FALSE;
        arg->value = arg->size == 2 ? m68k_read16(ctx, addr) : m68k_read32(ctx, addr);
        if (arg->size == 8)
            arg->value = (arg->value << 32) | m68k_read32(ctx, addr + 4);
        addr += arg->size;
        value = (ULONG)arg->value;
        if (arg->kind && value)
        {
            if (arg->kind == 's')
            {
                const char *s = guest_string(ctx, value);
                if (!s) return FALSE;
                arg->body = (const UBYTE *)s;
                arg->length = (ULONG)strlen(s);
            }
            else
            {
                if (value > UINT32_MAX / 4) return FALSE;
                value *= 4;
                if (value >= ctx->mem_size || ctx->mem_size - value < 1) return FALSE;
                arg->length = ctx->mem[value];
                if (value >= ctx->mem_size || ctx->mem_size - value < arg->length + 1)
                    return FALSE;
                arg->body = ctx->mem + value + 1;
            }
        }
    }
    *next = addr;
    return TRUE;
}

static BOOL locale_format_body(const struct Locale *locale,
                               const struct LocaleFormatField *field,
                               const struct LocaleFormatArg *arg,
                               UBYTE *digits, const UBYTE **body, ULONG *length)
{
    uint64_t value = arg ? arg->value : 0;
    UBYTE *end = digits + 64, *p = end;
    BOOL minus = FALSE;
    ULONG base, group = 0, group_length = 0, separator_length = 0;
    UBYTE groupsize = 255;
    const char *hex = field->code == 'X' || field->code == 'p' ?
                      "0123456789abcdef" : "0123456789ABCDEF";

    if (!arg || field->code == 'c')
    {
        digits[0] = arg ? (UBYTE)value : field->code;
        *body = digits;
        *length = 1;
        return TRUE;
    }
    if (arg->kind)
    {
        *body = arg->body;
        *length = arg->length;
        return TRUE;
    }
    base = field->code == 'd' || field->code == 'D' ||
           field->code == 'u' || field->code == 'U' ? 10 : 16;
    if (field->code == 'd' || field->code == 'D')
    {
        minus = (value & (UINT64_C(1) << (field->size * 8 - 1))) != 0;
        if (minus)
        {
            value = UINT64_C(0) - value;
            if (field->size < 8) value &= (UINT64_C(1) << (field->size * 8)) - 1;
        }
    }
    if (field->code == 'D' || field->code == 'U')
    {
        if (!locale->loc_Grouping || !locale->loc_GroupSeparator) return FALSE;
        group_length = (ULONG)strlen((const char *)locale->loc_Grouping);
        separator_length = (ULONG)strlen((const char *)locale->loc_GroupSeparator);
        groupsize = locale->loc_Grouping[0];
    }
    do
    {
        *--p = (UBYTE)hex[value % base];
        value /= base;
        if (base == 10 && --groupsize == 0 && value)
        {
            /* AROS selects one separator byte per grouping position and
               repeats the previous group when the next size is zero. */
            if (group >= group_length || group > separator_length) return FALSE;
            *--p = (UBYTE)locale->loc_GroupSeparator[group];
            groupsize = locale->loc_Grouping[group + 1];
            if (!groupsize) groupsize = locale->loc_Grouping[group];
            else group++;
        }
    } while (value);
    if (minus) *--p = '-';
    *body = p;
    *length = (ULONG)(end - p);
    return TRUE;
}

static BOOL locale_format_render(const struct Locale *locale, const char *format,
                                 const struct LocaleFormatArg *args,
                                 UBYTE *output, ULONG *size)
{
    const char *p = format;
    ULONG ordinal = 0, total = 0;

    while (*p)
    {
        struct LocaleFormatField field;
        const UBYTE *body;
        UBYTE digits[64];
        ULONG length, padding;

        if (*p++ != '%')
        {
            if (total == LOCALE_FMT_MAX - 1) return FALSE;
            if (output) output[total] = (UBYTE)p[-1];
            total++;
            continue;
        }
        if (!locale_format_field(&p, &ordinal, &field) ||
            !locale_format_body(locale, &field,
                field.position ? &args[field.position - 1] : NULL,
                digits, &body, &length)) return FALSE;
        if (length > field.limit) length = field.limit;
        padding = field.width > length ? field.width - length : 0;
        if (length >= LOCALE_FMT_MAX - total ||
            padding >= LOCALE_FMT_MAX - total - length) return FALSE;
        if (output)
        {
            if (!field.left) memset(output + total, field.fill, padding);
            if (length) memcpy(output + total + (field.left ? 0 : padding), body, length);
            if (field.left) memset(output + total + length, field.fill, padding);
        }
        total += length + padding;
    }
    if (output) output[total] = 0;
    *size = total + 1;
    return TRUE;
}

/* -66: FormatString(A0=locale, A1=template, A2=dataStream, A3=putCharFunc) -> D0 */
static IPTR kaffeine_locale_FormatString(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale = THUNK_A(0), stream = THUNK_A(2), format_size = 0;
    ULONG ordinal = 0, count = 0, args_size = 0, output_size = 0, next = stream, i;
    int slot = find_locale_slot(ctx, locale);
    struct KaffeineNativeHook hook;
    struct LocaleHookData hook_args;
    struct LocaleFormatField field;
    struct LocaleFormatArg *args = NULL;
    const char *source, *p;
    char *format = NULL;
    UBYTE *output = NULL;
    IPTR result = 0;

    /* As in AROS, a NULL template does nothing and returns the data stream. */
    if (!THUNK_A(1)) return stream;
    if (locale && slot < 0) return 0;
    hook_args.native_locale = NULL;
    hook_args.guest_locale = locale;
    hook_args.input = FALSE;
    if (!Kaffeine_InitNativeHook(&hook, ctx, cpu, THUNK_A(3), locale_hook_args, &hook_args))
        return 0;
    source = guest_string(ctx, THUNK_A(1));
    if (!source) return 0;
    format_size = (ULONG)strlen(source) + 1;
    format = AllocMem(format_size, MEMF_ANY);
    if (!format) return 0;
    memcpy(format, source, format_size);
    for (p = format; *p;)
    {
        if (*p++ != '%') continue;
        if (!locale_format_field(&p, &ordinal, &field)) goto done;
        if (field.position > count) count = field.position;
    }
    if (count > ordinal || count > LOCALE_FMT_MAX / sizeof(*args)) goto done;
    if (count)
    {
        args_size = count * sizeof(*args);
        args = AllocMem(args_size, MEMF_ANY);
        if (!args) goto done;
        memset(args, 0, args_size);
    }
    if (!locale_format_args(ctx, format, stream, args, count, &next)) goto done;
    hook_args.native_locale = enter_locale_call(ctx, slot);
    if (!hook_args.native_locale) goto done;
    if (!locale_format_render(hook_args.native_locale, format, args, NULL, &output_size))
        goto leave;
    output = AllocMem(output_size, MEMF_ANY);
    if (!output) goto leave;
    if (!locale_format_render(hook_args.native_locale, format, args, output, &output_size))
        goto leave;

    for (i = 0; i < output_size; i++)
    {
        AROS_UFC3NR(VOID, hook.hook.h_Entry,
            AROS_UFCA(struct Hook *, &hook.hook, A0),
            AROS_UFCA(const struct Locale *, hook_args.native_locale, A2),
            AROS_UFCA(UBYTE, output[i], A1));
        if (hook.failed || !ctx->running) break;
    }
    result = hook.failed || !ctx->running ? (IPTR)ctx->exit_code : (IPTR)next;
leave:
    leave_locale_call(ctx, slot, hook_args.native_locale);
done:
    if (output) FreeMem(output, output_size);
    if (args) FreeMem(args, args_size);
    if (format) FreeMem(format, format_size);
    return result;
}

/* -162: ParseDate(A0=locale, A1=date, A2=template, A3=getCharFunc) -> D0 */
static IPTR kaffeine_locale_ParseDate(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale = THUNK_A(0), date_addr = THUNK_A(1), size;
    int slot = find_locale_slot(ctx, locale);
    struct DateStamp date = {0, 0, 0};
    struct KaffeineNativeHook hook;
    struct LocaleHookData args;
    char *format;
    BOOL result;

    if ((locale && slot < 0) || !guest_date_range(ctx, date_addr)) return 0;
    args.native_locale = NULL;
    args.guest_locale = locale;
    args.input = TRUE;
    if (!Kaffeine_InitNativeHook(&hook, ctx, cpu, THUNK_A(3), locale_hook_args, &args))
        return 0;
    format = copy_date_template(ctx, THUNK_A(2), &size);
    if (!format) return 0;

    result = FALSE;
    args.native_locale = enter_locale_call(ctx, slot);
    if (args.native_locale)
    {
        result = ParseDate(args.native_locale, &date, (CONST_STRPTR)format, &hook.hook);
        leave_locale_call(ctx, slot, args.native_locale);
    }
    FreeMem(format, size);
    if (hook.failed || !ctx->running) return (IPTR)ctx->exit_code;
    if (result)
    {
        m68k_write32(ctx, date_addr, (ULONG)date.ds_Days);
        m68k_write32(ctx, date_addr + 4, (ULONG)date.ds_Minute);
        m68k_write32(ctx, date_addr + 8, (ULONG)date.ds_Tick);
    }
    return (IPTR)result;
}

/* -72: GetCatalogStr(A0=catalog, D0=stringNum, A1=defaultString) -> D0 */
static IPTR kaffeine_locale_GetCatalogStr(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    return (IPTR)THUNK_A(1);
}

/* -78: GetLocaleStr(A0=locale, D0=stringNum) -> D0 */
static IPTR kaffeine_locale_GetLocaleStr(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG string_num = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);
    CONST_STRPTR native_str;
    ULONG len, copy_addr;

    if (slot < 0 || string_num >= KAFFEINE_LOCALE_MAXSTRMSG)
        return 0;

    if (ctx->locale_map[slot].str_addrs[string_num])
        return (IPTR)ctx->locale_map[slot].str_addrs[string_num];

    native_str = GetLocaleStr((struct Locale *)ctx->locale_map[slot].native_locale,
                              string_num);
    if (!native_str) return 0;

    len = (ULONG)strlen((const char *)native_str) + 1;
    copy_addr = Kaffeine_LocaleAlloc(ctx, len);
    if (!copy_addr) return 0;

    memcpy(ctx->mem + copy_addr, native_str, len);
    ctx->locale_map[slot].str_addrs[string_num] = copy_addr;
    return (IPTR)copy_addr;
}

/* -84: IsAlNum(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_IsAlNum(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return 0;
    return (IPTR)IsAlNum((struct Locale *)ctx->locale_map[slot].native_locale,
                         character);
}

/* -90: IsAlpha(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_IsAlpha(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return 0;
    return (IPTR)IsAlpha((struct Locale *)ctx->locale_map[slot].native_locale,
                         character);
}

/* -96: IsCntrl(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_IsCntrl(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return 0;
    return (IPTR)IsCntrl((struct Locale *)ctx->locale_map[slot].native_locale,
                         character);
}

/* -102: IsDigit(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_IsDigit(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return 0;
    return (IPTR)IsDigit((struct Locale *)ctx->locale_map[slot].native_locale,
                         character);
}

/* -108: IsGraph(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_IsGraph(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return 0;
    return (IPTR)IsGraph((struct Locale *)ctx->locale_map[slot].native_locale,
                         character);
}

/* -114: IsLower(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_IsLower(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return 0;
    return (IPTR)IsLower((struct Locale *)ctx->locale_map[slot].native_locale,
                         character);
}

/* -120: IsPrint(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_IsPrint(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return 0;
    return (IPTR)IsPrint((struct Locale *)ctx->locale_map[slot].native_locale,
                         character);
}

/* -126: IsPunct(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_IsPunct(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return 0;
    return (IPTR)IsPunct((struct Locale *)ctx->locale_map[slot].native_locale,
                         character);
}

/* -132: IsSpace(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_IsSpace(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return 0;
    return (IPTR)IsSpace((struct Locale *)ctx->locale_map[slot].native_locale,
                         character);
}

/* -138: IsUpper(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_IsUpper(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return 0;
    return (IPTR)IsUpper((struct Locale *)ctx->locale_map[slot].native_locale,
                         character);
}

/* -144: IsXDigit(A0=locale, D0=character) -> D0 */
static IPTR kaffeine_locale_IsXDigit(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG character = THUNK_D(0);
    int slot = find_locale_slot(ctx, locale_addr);

    if (slot < 0) return 0;
    return (IPTR)IsXDigit((struct Locale *)ctx->locale_map[slot].native_locale,
                          character);
}

/* -150: OpenCatalogA(A0=locale, A1=name, A2=tags) -> D0 */
static IPTR kaffeine_locale_OpenCatalogA(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    (void)cpu;
    return 0;
}

/* -156: OpenLocale(A0=name) -> D0 */
static IPTR kaffeine_locale_OpenLocale(struct KaffeineContext *ctx, void *cpu)
{
    ULONG name_addr = THUNK_A(0);
    const char *name = NULL;
    int slot;
    struct Locale *native_loc;
    ULONG guest_addr;

    if (name_addr != 0)
    {
        name = guest_string(ctx, name_addr);
        if (!name) return 0;
    }

    slot = find_free_locale_slot(ctx);
    if (slot < 0) return 0;

    native_loc = OpenLocale((CONST_STRPTR)name);
    if (!native_loc) return 0;

    guest_addr = build_guest_locale(ctx, native_loc);
    if (!guest_addr)
    {
        CloseLocale(native_loc);
        return 0;
    }

    ctx->locale_map[slot].guest_addr = guest_addr;
    ctx->locale_map[slot].native_locale = native_loc;
    memset(ctx->locale_map[slot].str_addrs, 0,
           sizeof(ctx->locale_map[slot].str_addrs));
    return (IPTR)guest_addr;
}

/* -174: StrConvert(A0=locale, A1=string, A2=buffer, D0=bufferSize, D1=type) -> D0 */
static IPTR kaffeine_locale_StrConvert(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG string_addr = THUNK_A(1);
    ULONG buffer_addr = THUNK_A(2);
    ULONG buffer_size = THUNK_D(0);
    ULONG type = THUNK_D(1);
    int slot = find_locale_slot(ctx, locale_addr);
    const char *str;
    APTR buf = NULL;

    if (slot < 0) return 0;
    str = guest_string(ctx, string_addr);
    if (!str) return 0;

    if (buffer_size > 0)
    {
        if (!buffer_addr || buffer_addr >= ctx->mem_size ||
            ctx->mem_size - buffer_addr < buffer_size)
            return 0;
        buf = ctx->mem + buffer_addr;
    }

    return (IPTR)StrConvert((struct Locale *)ctx->locale_map[slot].native_locale,
                            (CONST_STRPTR)str, buf, buffer_size, type);
}

/* -180: StrnCmp(A0=locale, A1=string1, A2=string2, D0=length, D1=type) -> D0 */
static IPTR kaffeine_locale_StrnCmp(struct KaffeineContext *ctx, void *cpu)
{
    ULONG locale_addr = THUNK_A(0);
    ULONG s1_addr = THUNK_A(1);
    ULONG s2_addr = THUNK_A(2);
    LONG length = (LONG)THUNK_D(0);
    ULONG type = THUNK_D(1);
    int slot = find_locale_slot(ctx, locale_addr);
    const char *s1, *s2;

    if (slot < 0) return 0;
    s1 = guest_string(ctx, s1_addr);
    s2 = guest_string(ctx, s2_addr);
    if (!s1 || !s2) return 0;

    return (IPTR)StrnCmp((struct Locale *)ctx->locale_map[slot].native_locale,
                         (CONST_STRPTR)s1, (CONST_STRPTR)s2, length, type);
}

const struct M68KThunkEntry kaffeine_thunks_locale_manual[] =
{
    {  36, kaffeine_locale_CloseCatalog  },
    {  42, kaffeine_locale_CloseLocale   },
    {  48, kaffeine_locale_ConvToLower   },
    {  54, kaffeine_locale_ConvToUpper   },
    {  60, kaffeine_locale_FormatDate    },
    {  66, kaffeine_locale_FormatString  },
    {  72, kaffeine_locale_GetCatalogStr },
    {  78, kaffeine_locale_GetLocaleStr  },
    {  84, kaffeine_locale_IsAlNum       },
    {  90, kaffeine_locale_IsAlpha       },
    {  96, kaffeine_locale_IsCntrl       },
    { 102, kaffeine_locale_IsDigit       },
    { 108, kaffeine_locale_IsGraph       },
    { 114, kaffeine_locale_IsLower       },
    { 120, kaffeine_locale_IsPrint       },
    { 126, kaffeine_locale_IsPunct       },
    { 132, kaffeine_locale_IsSpace       },
    { 138, kaffeine_locale_IsUpper       },
    { 144, kaffeine_locale_IsXDigit      },
    { 150, kaffeine_locale_OpenCatalogA  },
    { 156, kaffeine_locale_OpenLocale    },
    { 162, kaffeine_locale_ParseDate     },
    { 174, kaffeine_locale_StrConvert    },
    { 180, kaffeine_locale_StrnCmp       },
    {   0, NULL                         },
};
const ULONG kaffeine_thunks_locale_manual_count =
    sizeof(kaffeine_thunks_locale_manual) / sizeof(kaffeine_thunks_locale_manual[0]) - 1;
