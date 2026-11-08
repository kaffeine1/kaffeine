/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Retired 68k Screen addresses of a context. An IEPointerPixel names a
    screen by the 32-bit address of its 68k Screen, which a program may keep
    in a register or a variable after the screen has gone. So the block of a
    68k Screen that the screen bridge or an IBase snapshot made never goes
    back to the guest heap while the context lives: its bytes become 0 and
    the address is booked here, with the native screen and the registry
    generation it stood for. Only that very screen, the same native with the
    same generation and a block of the same size, may take the address
    again, as LockPubScreen() hands out the same 68k Screen of a screen each
    time. The book keeps the latest KAFFEINE_SCREEN_BOOK of them; an older
    one stays retired without its way back, still allocated. The memory is
    the guest heap's, counted there, and goes with the context.
*/
#ifndef KAFFEINE_SCREEN_BOOK_H
#define KAFFEINE_SCREEN_BOOK_H

#include "kaffeine_intern.h"
#include <string.h>

enum
{
    KAFFEINE_BOOK_BRIDGE = 1, /* the 68k Screen of the screen bridge */
    KAFFEINE_BOOK_VIEW = 2    /* a Screen view of an IBase snapshot */
};

static inline void Kaffeine_ScreenRetire(struct KaffeineContext *ctx, UBYTE kind, const void *native,
                                         ULONG generation, ULONG guest, ULONG size)
{
    UWORD n = ctx->screen_book_count;

    if (!guest || !size)
        return;
    memset(ctx->mem + guest, 0, size);
    if (n == KAFFEINE_SCREEN_BOOK)
    {
        memmove(&ctx->screen_book[0], &ctx->screen_book[1],
                (KAFFEINE_SCREEN_BOOK - 1) * sizeof(ctx->screen_book[0]));
        n--;
    }
    ctx->screen_book[n].native = native;
    ctx->screen_book[n].guest = guest;
    ctx->screen_book[n].size = size;
    ctx->screen_book[n].generation = generation;
    ctx->screen_book[n].kind = kind;
    ctx->screen_book_count = n + 1;
}

/* The retired address of exactly this screen, taken out of the book, or 0 */
static inline ULONG Kaffeine_ScreenRevive(struct KaffeineContext *ctx, UBYTE kind, const void *native,
                                          ULONG generation, ULONG size)
{
    UWORD i, n = ctx->screen_book_count;

    for (i = n; i-- > 0;)
        if (ctx->screen_book[i].kind == kind && ctx->screen_book[i].native == native &&
            ctx->screen_book[i].generation == generation && ctx->screen_book[i].size == size)
        {
            ULONG guest = ctx->screen_book[i].guest;

            memmove(&ctx->screen_book[i], &ctx->screen_book[i + 1],
                    (n - i - 1) * sizeof(ctx->screen_book[0]));
            ctx->screen_book_count = n - 1;
            return guest;
        }
    return 0;
}

#endif
