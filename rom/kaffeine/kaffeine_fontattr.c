/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Convert the public 68k TextAttr ABI without forwarding native pointers.
*/
#include <string.h>

#include "kaffeine_intern.h"
#include "kaffeine_fontattr.h"
#include "kaffeine_taglist.h"

BOOL Kaffeine_FontAttrIn(struct KaffeineContext *ctx, ULONG guest,
                        struct KaffeineFontAttr *input)
{
    struct KaffeineTagWalk walk;
    ULONG name, list, item, count = 0;
    int result;

    memset(input, 0, sizeof(*input));
    if (!ctx->mem || !guest || guest >= ctx->mem_size || ctx->mem_size - guest < 8)
        return FALSE;
    name = m68k_read32(ctx, guest);
    if (!name || name >= ctx->mem_size ||
        !memchr(ctx->mem + name, 0, ctx->mem_size - name))
        return FALSE;
    input->attr.tta_Name = (STRPTR)(ctx->mem + name);
    input->attr.tta_YSize = m68k_read16(ctx, guest + 4);
    input->attr.tta_Style = ctx->mem[guest + 6];
    input->attr.tta_Flags = ctx->mem[guest + 7];
    if (!(input->attr.tta_Style & FSF_TAGGED))
        return TRUE;
    if (ctx->mem_size - guest < 12)
        return FALSE;
    list = m68k_read32(ctx, guest + 8);
    if (!list)
        return TRUE;
    Kaffeine_TagWalkInit(&walk, list);
    while ((result = Kaffeine_TagWalkNext(ctx, &walk, &item)) == KAFFEINE_TAG_ITEM)
    {
        ULONG tag = m68k_read32(ctx, item);

        /* bullet.c reads this as two unsigned 16-bit DPI components.
           Never guess whether the data of an unknown tag is a pointer. */
        if (tag != TA_DeviceDPI || count == KAFFEINE_FONTATTR_TAGS)
            return FALSE;
        input->tags[count].ti_Tag = tag;
        input->tags[count++].ti_Data = (IPTR)m68k_read32(ctx, item + 4);
    }
    if (result != KAFFEINE_TAG_END)
        return FALSE;
    input->tags[count].ti_Tag = TAG_DONE;
    input->attr.tta_Tags = input->tags;
    return TRUE;
}
