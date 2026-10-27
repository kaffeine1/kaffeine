#ifndef KAFFEINE_TEST_NATIVE_EXECBASE_H
#define KAFFEINE_TEST_NATIVE_EXECBASE_H
#include <exec/libraries.h>
/* Native 64-bit ExecBase embeds List headers with complete pointer fields. */
struct List
{
    struct Node *lh_Head, *lh_Tail, *lh_TailPred;
    UBYTE lh_Type, l_pad;
};
/* Enough native storage for every generated ExecBase field (through 1303). */
struct ExecBase
{
    struct Library LibNode;
    UBYTE snapshot[1240];
};
#endif
