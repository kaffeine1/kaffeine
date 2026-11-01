/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
/* The three native ASL calls of the file requester bridge, recorded by
   test_asl.cpp. */
#pragma once
#include <libraries/asl.h>
#ifdef __cplusplus
extern "C" {
#endif
APTR AllocAslRequest(ULONG reqType, struct TagItem *tagList);
BOOL AslRequest(APTR requester, struct TagItem *tagList);
void FreeAslRequest(APTR requester);
#ifdef __cplusplus
}
#endif
