#pragma once
#include <exec/types.h>
/* The fields of a native Library that the exec I/O bridge reads. */
struct Library
{
    UWORD lib_Version;
    UWORD lib_Revision;
    char pad[30];
};
