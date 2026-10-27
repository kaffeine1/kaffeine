/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include <devices/inputevent.h>
struct KeyMap;
#ifdef __cplusplus
extern "C" {
#endif
WORD MapRawKey(struct InputEvent *event, STRPTR buffer, LONG length, struct KeyMap *keyMap);
#ifdef __cplusplus
}
#endif
