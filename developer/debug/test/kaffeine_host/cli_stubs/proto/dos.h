#ifndef KAFFEINE_TEST_CLI_PROTO_DOS_H
#define KAFFEINE_TEST_CLI_PROTO_DOS_H

#include <exec/types.h>
#define ERROR_NO_FREE_STORE 103
#define ERROR_BAD_NUMBER 115
#define ERROR_OBJECT_WRONG_TYPE 212

struct CommandLineInterface;

#ifdef __cplusplus
extern "C" {
#endif

struct CommandLineInterface *Cli(void);
SIPTR SetIoErr(SIPTR error);

#ifdef __cplusplus
}
#endif

#endif
