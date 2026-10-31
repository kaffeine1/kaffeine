/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Run natively after gfx_owned_screen leave in the same boot. */
#include <exec/types.h>
#include <intuition/screens.h>
#include <proto/intuition.h>
#include <stdio.h>
int main(void)
{
    struct Screen *screen=LockPubScreen((CONST_STRPTR)"KAFFEINE-OWNED-TEARDOWN");
    if(screen)UnlockPubScreen(NULL,screen);
    printf("gfx_screen_cleanup_native: 1 checks, %u failed\n",screen?1u:0u);
    return screen?10:0;
}
