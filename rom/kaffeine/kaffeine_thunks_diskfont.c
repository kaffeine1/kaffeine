/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    diskfont.library uses the shared graphics font identity and ownership.
*/
#include <aros/debug.h>
#include <proto/diskfont.h>

#include "kaffeine_intern.h"
#include "kaffeine_fontattr.h"
#include "kaffeine_rastport.h"
#include "kaffeine_thunks_diskfont.h"

/* NDK diskfont -30: OpenDiskFont(A0=textAttr) -> D0=68k TextFont.
   AROS temporarily changes ta_Name, so even an untagged request needs a
   native local object rather than a cast into the guest arena. */
IPTR thunk_diskfont_OpenDiskFont(struct KaffeineContext *ctx, void *cpu)
{
    struct KaffeineFontAttr input;
    struct TextFont *native;
    ULONG guest;

    if (!Kaffeine_FontAttrIn(ctx, THUNK_A(0), &input))
    {
        bug("[kaffeine] OpenDiskFont: invalid or unsupported TextAttr/tags\n");
        return 0;
    }
    bug("[kaffeine] OpenDiskFont name=\"%.256s\" size=%u style=%02x flags=%02x\n",
        input.attr.tta_Name, (unsigned)input.attr.tta_YSize,
        (unsigned)input.attr.tta_Style, (unsigned)input.attr.tta_Flags);
    native = OpenDiskFont((struct TextAttr *)&input.attr);
    if (native)
        bug("[kaffeine] OpenDiskFont native name=\"%.256s\" size=%u style=%02x flags=%02x\n",
            native->tf_Message.mn_Node.ln_Name ?
                (const char *)native->tf_Message.mn_Node.ln_Name : "(unnamed)",
            (unsigned)native->tf_YSize, (unsigned)native->tf_Style,
            (unsigned)native->tf_Flags);
    guest = Kaffeine_FontAdopt(ctx, native);
    bug("[kaffeine] OpenDiskFont guest=%08lx\n", (unsigned long)guest);
    return guest;
}

const struct M68KThunkEntry kaffeine_thunks_diskfont_manual[] = {
    { 30, thunk_diskfont_OpenDiskFont },
    { 0, NULL }
};
const ULONG kaffeine_thunks_diskfont_manual_count =
    sizeof(kaffeine_thunks_diskfont_manual) / sizeof(kaffeine_thunks_diskfont_manual[0]) - 1;
