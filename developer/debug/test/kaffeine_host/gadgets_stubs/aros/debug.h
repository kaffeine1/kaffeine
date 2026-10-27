#pragma once
#define D(x)
#ifdef __cplusplus
extern "C" {
#endif
void kaffeine_gadget_test_debug(const char *format, ...);
#ifdef __cplusplus
}
#endif
#define bug kaffeine_gadget_test_debug
