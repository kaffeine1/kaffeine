#pragma once
#include <exec/types.h>

typedef ULONG Tag;

struct TagItem
{
    Tag  ti_Tag;
    IPTR ti_Data;
};

#define TAG_DONE   (0UL)
#define TAG_END    (0UL)
#define TAG_IGNORE (1UL)
#define TAG_MORE   (2UL)
#define TAG_SKIP   (3UL)
#define TAG_USER   (1UL << 31)
#define TAGFILTER_AND 0
#define TAGFILTER_NOT 1
#define MAP_REMOVE_NOT_FOUND 0
#define MAP_KEEP_NOT_FOUND 1
