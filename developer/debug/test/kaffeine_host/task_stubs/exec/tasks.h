#ifndef KAFFEINE_TEST_NATIVE_TASKS_H
#define KAFFEINE_TEST_NATIVE_TASKS_H
#include <exec/types.h>

/* Prefix and storage for the generated 64-bit Task snapshot layout. */
struct Node
{
    struct Node *ln_Succ, *ln_Pred;
    UBYTE ln_Type;
    signed char ln_Pri;
    char *ln_Name;
};
struct Task
{
    struct Node tc_Node;
    UBYTE snapshot[144];
};
#define NT_TASK 1
#define NT_MSGPORT 4
#define NT_PROCESS 13
#define NT_SIGNALSEM 15
#endif
