#ifndef KAFFEINE_TEST_NATIVE_PROTO_DOS_H
#define KAFFEINE_TEST_NATIVE_PROTO_DOS_H
#include <dos/bptr.h>
#ifdef __cplusplus
extern "C" {
#endif
BPTR Input(void);
BPTR Output(void);
#ifdef __cplusplus
}
#endif
#endif
