/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    RawDoFmt rules from rom/exec/rawdofmt.c, using checked big-endian guest
    arguments. Keep the separate DOS argument converters unchanged.
*/
#include <stdint.h>
#include <string.h>
#include <proto/exec.h>
#include "kaffeine_intern.h"
#include "kaffeine_callback.h"
#include "kaffeine_format.h"

/* exec/rawfmt.h assigns STRING=0, SERIAL=1 and COUNT=2. They are guest
   constants here, not host function pointers. */
#define FMT_STRING 0u
#define FMT_SERIAL 1u
#define FMT_COUNT  2u
#define FMT_MAX    0x7FFFFFFFu

typedef BOOL (*FormatPut)(void *state, UBYTE ch);

static BOOL format_range(struct KaffeineContext *ctx, ULONG addr, ULONG size)
{
    return !(addr >= ctx->mem_size || ctx->mem_size - addr < size);
}

static BOOL format_string(struct KaffeineContext *ctx, ULONG addr, ULONG *length)
{
    const UBYTE *end;
    if (!addr || !format_range(ctx, addr, 1)) return FALSE;
    end = (const UBYTE *)memchr(ctx->mem + addr, 0, ctx->mem_size - addr);
    if (!end) return FALSE;
    *length = (ULONG)(end - (ctx->mem + addr));
    return TRUE;
}

static BOOL format_arg(struct KaffeineContext *ctx, ULONG *args,
                       ULONG size, ULONG *value)
{
    if (!*args || (*args & 1) || !format_range(ctx, *args, size)) return FALSE;
    *value = size == 2 ? m68k_read16(ctx, *args) : m68k_read32(ctx, *args);
    *args += size;
    return TRUE;
}

static BOOL format_decimal(struct KaffeineContext *ctx, ULONG *pos, ULONG *value)
{
    for (;;)
    {
        ULONG digit;
        if (!format_range(ctx, *pos, 1)) return FALSE;
        digit = ctx->mem[*pos];
        if (digit < '0' || digit > '9') return TRUE;
        digit -= '0';
        if (*value > (FMT_MAX - digit) / 10) return FALSE;
        *value = *value * 10 + digit;
        (*pos)++;
    }
}

static BOOL format_repeat(FormatPut put, void *state, UBYTE ch, ULONG count)
{
    if (put)
        while (count--)
            if (!put(state, ch)) return FALSE;
    return TRUE;
}

/* A count-only pass validates everything before output. The emitting pass
   rechecks every read: a guest PutChProc may modify the remaining input. */
static BOOL guest_format(struct KaffeineContext *ctx, ULONG fmt, ULONG args,
                         FormatPut put, void *state, ULONG *next, ULONG *count)
{
    ULONG pos = fmt, length, total = 0;
    if (!format_string(ctx, fmt, &length)) return FALSE;
    for (;;)
    {
        UBYTE ch;
        if (!format_range(ctx, pos, 1)) return FALSE;
        ch = ctx->mem[pos++];
        if (!ch) break;
        if (ch != '%')
        {
            if (total == FMT_MAX || (put && !put(state, ch))) return FALSE;
            total++;
        }
        else
        {
            BOOL left = FALSE, minus = FALSE;
            UBYTE fill = ' ', size = 'h', digits[10], *body = digits;
            ULONG minwidth = 0, maxwidth = FMT_MAX, width = 0;
            ULONG value = 0, guest_body = 0, padding, field, i;
            /* Every parser lookahead is checked, even after a callback
               removes a terminator validated by the initial scan. */
#define PEEK() do { if (!format_range(ctx, pos, 1)) return FALSE; ch = ctx->mem[pos]; } while (0)
            PEEK();
            if (ch == '-') { left = TRUE; pos++; PEEK(); }
            if (ch == '0') { fill = '0'; pos++; }
            if (!format_decimal(ctx, &pos, &minwidth)) return FALSE;
            PEEK();
            if (ch == '.')
            {
                pos++; PEEK();
                if (ch >= '0' && ch <= '9')
                {
                    maxwidth = 0;
                    if (!format_decimal(ctx, &pos, &maxwidth)) return FALSE;
                }
            }
            PEEK();
            if (ch == 'l' || ch == 'i') { size = ch; pos++; PEEK(); }
            if (ch == 's' || ch == 'b')
            {
                if (!format_arg(ctx, &args, 4, &value)) return FALSE;
                if (value)
                {
                    if (ch == 's')
                    {
                        if (!format_string(ctx, value, &width)) return FALSE;
                        guest_body = value;
                    }
                    else
                    {
                        if (value > UINT32_MAX / 4) return FALSE;
                        value *= 4;
                        if (!format_range(ctx, value, 1)) return FALSE;
                        width = ctx->mem[value];
                        if (!format_range(ctx, value, width + 1)) return FALSE;
                        guest_body = value + 1;
                    }
                }
            }
            else if (ch == 'd' || ch == 'D' || ch == 'u' || ch == 'U' ||
                     ch == 'x' || ch == 'X' || ch == 'p' || ch == 'P' || ch == 'c')
            {
                ULONG bytes = size == 'h' ? 2 : 4;
                if (ch == 'p' || ch == 'P') { fill = '0'; minwidth = 8; bytes = 4; }
                if (!format_arg(ctx, &args, bytes, &value)) return FALSE;
                if (ch == 'c') { digits[0] = (UBYTE)value; width = 1; }
                else
                {
                    ULONG base = (ch == 'd' || ch == 'D' || ch == 'u' || ch == 'U') ? 10 : 16;
                    if (ch == 'd' || ch == 'D')
                    {
                        if (bytes == 2) value = (ULONG)(int32_t)(int16_t)value;
                        minus = (value & 0x80000000u) != 0;
                        if (minus) value = 0u - value;
                    }
                    body = digits + sizeof(digits);
                    do
                    {
                        *--body = (UBYTE)"0123456789ABCDEF"[value % base];
                        value /= base;
                        width++;
                    } while (value);
                }
            }
            else if (ch) { digits[0] = ch; width = 1; }
            /* A trailing '%' formats an empty field and reuses the NUL. */
            if (ch) pos++;
            if (width > maxwidth) width = maxwidth;
            field = width + (ULONG)minus;
            padding = minwidth > field ? minwidth - field : 0;
            if (field > FMT_MAX - total || padding > FMT_MAX - total - field) return FALSE;
            total += field + padding;
            if (minus && fill != ' ' && put && !put(state, '-')) return FALSE;
            if (!left && !format_repeat(put, state, fill, padding)) return FALSE;
            if (minus && fill == ' ' && put && !put(state, '-')) return FALSE;
            if (put)
                for (i = 0; i < width; i++)
                {
                    UBYTE out;
                    if (guest_body)
                    {
                        if (!format_range(ctx, guest_body, 1)) return FALSE;
                        out = ctx->mem[guest_body++];
                    }
                    else out = body[i];
                    if (!put(state, out)) return FALSE;
                }
            if (left && !format_repeat(put, state, fill, padding)) return FALSE;
#undef PEEK
        }
    }
    if (total == FMT_MAX || (put && !put(state, 0))) return FALSE;
    *next = args;
    *count = total + 1;
    return TRUE;
}

struct RawOutput
{
    struct KaffeineContext *ctx;
    void *cpu;
    ULONG proc, data;
    BOOL copy;
};

static BOOL raw_put(void *state, UBYTE ch)
{
    struct RawOutput *out = (struct RawOutput *)state;
    struct KaffeineContext *ctx = out->ctx;
    if (!ctx->running) return FALSE;
    if (out->copy)
    {
        if (!out->data || !format_range(ctx, out->data, 1)) return FALSE;
        ctx->mem[out->data++] = ch;
    }
    else if (out->proc == FMT_SERIAL) RawPutChar(ch);
    else if (out->proc == FMT_COUNT)
    {
        if (!out->data || !format_range(ctx, out->data, 4)) return FALSE;
        m68k_write32(ctx, out->data, m68k_read32(ctx, out->data) + 1u);
    }
    else
    {
        struct KaffeineGuestCall call = { {0}, {0}, 1u, 1u << 3 };
        call.d[0] = ch;
        call.a[3] = out->data;
        if (!Kaffeine_CallGuest(ctx, out->cpu, out->proc, &call)) return FALSE;
        out->data = call.a[3];
    }
    return TRUE;
}

IPTR thunk_exec_RawDoFmt(struct KaffeineContext *ctx, void *cpu)
{
    ULONG fmt = THUNK_A(0), args = THUNK_A(1), next, count;
    struct RawOutput out = { ctx, cpu, THUNK_A(2), THUNK_A(3), FALSE };
    out.copy = out.proc == FMT_STRING;
    if (out.proc > FMT_COUNT)
    {
        if ((out.proc & 1) || !format_range(ctx, out.proc, 2)) goto fail;
        if (format_range(ctx, out.proc, 4) && m68k_read16(ctx, out.proc) == 0x16C0 &&
            m68k_read16(ctx, out.proc + 2) == 0x4E75) out.copy = TRUE;
    }
    if (!guest_format(ctx, fmt, args, NULL, NULL, &next, &count)) goto fail;
    if (out.copy && (!out.data || !format_range(ctx, out.data, count))) goto fail;
    if (out.proc == FMT_COUNT && (!out.data || !format_range(ctx, out.data, 4))) goto fail;
    if (!guest_format(ctx, fmt, args, raw_put, &out, &next, &count)) goto fail;
    return (IPTR)next;
fail:
    if (ctx->running) Kaffeine_StopGuest(ctx, cpu, 20);
    return (IPTR)ctx->exit_code;
}

BOOL Kaffeine_FormatToPut(struct KaffeineContext *ctx, ULONG fmt, ULONG args,
                         KaffeineFormatPut put, void *state)
{
    ULONG next;

    return Kaffeine_FormatToPutNext(ctx, fmt, args, put, state, &next);
}

BOOL Kaffeine_FormatToPutNext(struct KaffeineContext *ctx, ULONG fmt, ULONG args,
                             KaffeineFormatPut put, void *state, ULONG *next)
{
    ULONG count;

    /* The counting pass checks everything; the emitting pass rechecks
       each read, so output never depends on an unchecked byte. */
    return guest_format(ctx, fmt, args, NULL, NULL, next, &count) &&
           guest_format(ctx, fmt, args, put, state, next, &count);
}

BOOL Kaffeine_FormatCount(struct KaffeineContext *ctx, ULONG fmt, ULONG args,
                         ULONG *count, ULONG *next)
{
    return guest_format(ctx, fmt, args, NULL, NULL, next, count);
}

struct BufferOutput
{
    UBYTE *temp;
    ULONG capacity, used;
    BOOL terminated;
};

static BOOL buffer_put(void *state, UBYTE ch)
{
    struct BufferOutput *out = (struct BufferOutput *)state;
    /* AROS formats into a temporary and then Strlcpy()s it. An embedded
       zero therefore ends the copy, while counting still continues. */
    if (!ch) out->terminated = TRUE;
    if (!out->terminated && out->used < out->capacity - 1)
        out->temp[out->used++] = ch;
    return TRUE;
}

IPTR kaffeine_utility_VSNPrintf(struct KaffeineContext *ctx, void *cpu)
{
    ULONG buffer = THUNK_A(0), fmt = THUNK_A(1), args = THUNK_A(2), next, count, size;
    struct BufferOutput out = { NULL, THUNK_D(0), 0, FALSE };
    /* NULL buffers are count-only, also with size zero. A non-NULL buffer
       needs positive capacity, with its entire advertised range checked. */
    if (buffer && (!out.capacity || out.capacity > FMT_MAX ||
        !format_range(ctx, buffer, out.capacity))) return (IPTR)-1;
    if (!guest_format(ctx, fmt, args, NULL, NULL, &next, &count)) return (IPTR)-1;
    if (buffer)
    {
        /* As in AROS, format the whole result into a temporary before the
           copy: the destination may overlap the template, the data stream
           or a string argument. Without the temporary, AROS returns the
           count and leaves the buffer alone, and so does the bridge. */
        size = count < out.capacity ? count : out.capacity;
        out.temp = AllocMem(size, MEMF_ANY);
        if (!out.temp) return (IPTR)count;
        if (!guest_format(ctx, fmt, args, buffer_put, &out, &next, &count))
        {
            FreeMem(out.temp, size);
            return (IPTR)-1;
        }
        out.temp[out.used] = 0;
        memcpy(ctx->mem + buffer, out.temp, out.used + 1);
        FreeMem(out.temp, size);
    }
    return (IPTR)count;
}
