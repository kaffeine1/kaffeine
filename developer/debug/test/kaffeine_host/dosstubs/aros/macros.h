#pragma once
#include <exec/types.h>
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define AROS_BE2LONG(l) ((ULONG)(l))
#define AROS_BE2WORD(w) ((UWORD)(w))
#else
#define AROS_BE2LONG(l) __builtin_bswap32((ULONG)(l))
#define AROS_BE2WORD(w) __builtin_bswap16((UWORD)(w))
#endif
#define AROS_LONG2BE(l) AROS_BE2LONG(l)
#define AROS_WORD2BE(w) AROS_BE2WORD(w)
