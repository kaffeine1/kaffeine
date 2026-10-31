; Copyright (C) 2026, The AROS Development Team. All rights reserved.
; Author: Michele Dipace (@kaffeine1)
;
; 68k HUNK test program for the transparent kaffeine path on non-m68k AROS:
; LoadSeg() relocates it for its native addresses, RunCommand() hands it to
; kaffeine.library, which must rebase the copied segments. Every absolute
; operand below needs a relocation to the DATA or BSS hunk, so the message
; is printed only if the rebased copy is correct.

        section code,code

start:
        move.l  4.w,a6                  ; SysBase
        lea     dosname,a1              ; RELOC32 -> DATA
        moveq   #36,d0
        jsr     -552(a6)                ; OpenLibrary
        move.l  d0,dosbase              ; RELOC32 -> BSS
        beq.s   .nodos

        move.l  dosbase,a6              ; RELOC32 -> BSS, read back
        move.l  #message,d1             ; RELOC32 -> DATA
        jsr     -948(a6)                ; PutStr

        move.l  a6,a1
        move.l  4.w,a6
        jsr     -414(a6)                ; CloseLibrary
        moveq   #0,d0
        rts

.nodos:
        moveq   #20,d0
        rts

        section data,data

dosname:
        dc.b    "dos.library",0
message:
        dc.b    "kaffeine seglist test: relocated DATA and BSS reached",10,0

        section bss,bss

dosbase:
        ds.l    1
