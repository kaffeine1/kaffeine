#pragma once
#include <exec/types.h>
/* The StringExtend of a string gadget, laid out as in AROS */
struct TextFont;
struct Hook;
struct StringExtend
{
    struct TextFont *Font;
    UBYTE Pens[2];
    UBYTE ActivePens[2];
    ULONG InitialModes;
    struct Hook *EditHook;
    STRPTR WorkBuffer;
    ULONG Reserved[4];
};
