#ifndef KAFFEINE_TEST_NATIVE_LIBRARY_H
#define KAFFEINE_TEST_NATIVE_LIBRARY_H
#include <exec/tasks.h>
struct Library
{
    struct Node lib_Node;
    UBYTE lib_Flags, lib_pad;
    UWORD lib_NegSize, lib_PosSize, lib_Version, lib_Revision;
    char *lib_IdString;
    ULONG lib_Sum;
    UWORD lib_OpenCnt;
};
#endif
