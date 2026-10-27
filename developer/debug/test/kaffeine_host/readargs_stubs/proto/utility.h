#pragma once
/* Character case for ISO-8859-1, which the native parser needs */
#include <exec/types.h>

static inline UBYTE ToUpper(UBYTE c)
{
    return (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? c - 0x20 : c;
}

static inline UBYTE ToLower(UBYTE c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 0x20 : c;
}
