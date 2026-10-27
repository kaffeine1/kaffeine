#ifndef KAFFEINE_TEST_NATIVE_EXECBASE_H
#define KAFFEINE_TEST_NATIVE_EXECBASE_H
#include <exec/libraries.h>
/* Enough native storage for every generated ExecBase field (through 1303). */
struct ExecBase
{
    struct Library LibNode;
    UBYTE snapshot[1240];
};
#endif
