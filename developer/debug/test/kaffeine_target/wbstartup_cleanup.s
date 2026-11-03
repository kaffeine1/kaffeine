; Copyright (C) 2026, The AROS Development Team. All rights reserved.
; Author: Michele Dipace (@kaffeine1)
; Original cleanup controls: leave startup queued, or take without replying.
        section code,code
start:
        ifnd WBSTARTUP_UNTAKEN
        movem.l a2/a6,-(sp)
        move.l  4.w,a6
        suba.l  a1,a1
        jsr     -294(a6)
        move.l  d0,a2
        lea     92(a2),a2
        move.l  a2,a0
        jsr     -384(a6)
        move.l  a2,a0
        jsr     -372(a6)
        movem.l (sp)+,a2/a6
        endif
        moveq   #7,d0
        rts
