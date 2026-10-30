/* Native collaborators only; the production broker is compiled as C. */
#ifndef KAFFEINE_INTERN_H
#define KAFFEINE_INTERN_H
#include <exec/types.h>
#include <exec/tasks.h>
struct KaffeineContext {
    struct Task *exec_task;
    struct Task *broker_owner;
    BOOL broker_call;
    void (*after_thunk)(struct KaffeineContext *);
    ULONG guest_task, cpu_value, cleanup_count;
    void *fixture;
};
#endif
