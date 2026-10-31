/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#pragma once
#include <devices/inputevent.h>
struct KeyMap;
#ifdef __cplusplus
extern "C" {
#endif
WORD MapRawKey(struct InputEvent *event, STRPTR buffer, LONG length, struct KeyMap *keyMap);
void SetKeyMapDefault(struct KeyMap *keyMap);
struct KeyMap *AskKeyMapDefault(void);
LONG MapANSI(STRPTR string, LONG count, STRPTR buffer, LONG length, struct KeyMap *keyMap);
#ifdef __cplusplus
}
#endif
