/* Native collaborators only; the actual production runner is compiled as C. */
#ifndef KAFFEINE_INTERN_H
#define KAFFEINE_INTERN_H
#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/bptr.h>
struct KaffeineContext {
    char program_name[256];
    void (*root_complete)(struct KaffeineContext *,LONG);
    APTR root_completion_data;
    ULONG root_streams[3];
    char arguments[64];
    APTR fixture;
};
#endif
#ifndef KAFFEINE_FILEHANDLE_H
#define KAFFEINE_FILEHANDLE_H
#ifdef __cplusplus
extern "C" {
#endif
void Kaffeine_FileDropBorrowed(struct KaffeineContext *,ULONG);
#ifdef __cplusplus
}
#endif
#endif
