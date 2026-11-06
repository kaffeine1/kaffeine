/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   The fields of the native IntuitionBase that the IBase snapshot of
   kaffeine_intuibase.c reads, in the order of compiler/include/intuition/
   intuitionbase.h. LibNode carries only the two Library fields it copies:
   struct Library of these stubs is opaque. */
#pragma once
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>

struct IntuitionBase
{
    struct { UWORD lib_Version, lib_Revision; } LibNode;
    struct Window *ActiveWindow;
    struct Screen *ActiveScreen;
    struct Screen *FirstScreen;
    ULONG Flags;
    WORD MouseY, MouseX;
    ULONG Seconds, Micros;
};

#ifdef __cplusplus
extern "C" {
#endif
extern struct IntuitionBase *IntuitionBase;
#ifdef __cplusplus
}
#endif
