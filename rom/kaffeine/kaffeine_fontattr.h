/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Checked inputs shared by graphics OpenFont and diskfont OpenDiskFont.
*/
#ifndef KAFFEINE_FONTATTR_H
#define KAFFEINE_FONTATTR_H

#include <graphics/text.h>
#include <utility/tagitem.h>

struct KaffeineContext;

/* Only scalar TA_DeviceDPI is supported, with at most 64 effective items.
   Control tags are flattened after bounds and cycle validation. Unknown
   tags, including all pointer-bearing tags, fail before native code runs. */
#define KAFFEINE_FONTATTR_TAGS 64
struct KaffeineFontAttr
{
    struct TTextAttr attr;
    struct TagItem tags[KAFFEINE_FONTATTR_TAGS + 1];
};

#ifdef __cplusplus
extern "C" {
#endif

/* An 8-byte TextAttr, or 12-byte TTextAttr when FSF_TAGGED is set. The
   name remains a checked NUL-terminated guest byte string for native
   reads. All structures and tag items are native copies, including a
   NULL native tag pointer for a NULL guest list. No guest byte is written. */
BOOL Kaffeine_FontAttrIn(struct KaffeineContext *ctx, ULONG guest,
                        struct KaffeineFontAttr *input);

#ifdef __cplusplus
}
#endif
#endif
