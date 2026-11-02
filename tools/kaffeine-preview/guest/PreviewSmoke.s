; Copyright (C) 2026, The AROS Development Team. All rights reserved.
; Author: Michele Dipace (@kaffeine1)
;
; Original 68000 HUNK smoke test, no SDK headers or C runtime required.
; Build: vasmm68k_mot -quiet -m68000 -Fhunkexe -o PreviewSmoke PreviewSmoke.s
; The preview image creates SYS:KaffeinePreview/Results before execution.
; Return 0 only after save, close, reopen and exact byte/length comparison.
; Actual CPU backend selection is reported by kaffeine.library, not this file.

        section code,code
start:
        movem.l d2-d7/a2-a6,-(sp)
        moveq   #20,d7
        move.l  4.w,a6
        lea     dosname,a1
        moveq   #36,d0
        jsr     -552(a6)
        tst.l   d0
        beq     finish
        move.l  d0,a5
        move.l  a5,a6
        move.l  #intro,d1
        jsr     -948(a6)

        move.l  #filename,d1
        move.l  #1006,d2
        jsr     -30(a6)
        tst.l   d0
        beq     failed
        move.l  d0,d4
        move.l  d4,d1
        move.l  #payload,d2
        move.l  #payload_len,d3
        jsr     -48(a6)
        move.l  d0,d5
        move.l  d4,d1
        jsr     -36(a6)
        tst.l   d0
        beq     failed
        cmpi.l  #payload_len,d5
        bne     failed

        move.l  #filename,d1
        move.l  #1005,d2
        jsr     -30(a6)
        tst.l   d0
        beq     failed
        move.l  d0,d4
        move.l  d4,d1
        move.l  #readback,d2
        move.l  #payload_len+1,d3
        jsr     -42(a6)
        move.l  d0,d5
        move.l  d4,d1
        jsr     -36(a6)
        tst.l   d0
        beq     failed
        cmpi.l  #payload_len,d5
        bne     failed
        lea     payload,a0
        lea     readback,a1
        move.w  #payload_len-1,d0
compare:
        cmpm.b  (a0)+,(a1)+
        bne     failed
        dbra    d0,compare
        moveq   #0,d7
        move.l  #passed,d1
        bra     report
failed:
        move.l  #failure,d1
report:
        move.l  a5,a6
        jsr     -948(a6)
        move.l  a5,a1
        move.l  4.w,a6
        jsr     -414(a6)
finish:
        move.l  d7,d0
        movem.l (sp)+,d2-d7/a2-a6
        rts

        section data,data
version:
        dc.b    '$VER: KaffeinePreviewSmoke 0.1 (2.11.2026)',0
        even
dosname:
        dc.b    'dos.library',0
filename:
        dc.b    'SYS:KaffeinePreview/Results/smoke.txt',0
intro:
        dc.b    'Kaffeine 68k smoke: save, close, reopen, compare.',10,0
passed:
        dc.b    'PASS: 68k file saved and reloaded with identical bytes.',10,0
failure:
        dc.b    'FAIL: file operation or byte comparison failed.',10,0
payload:
        dc.b    'Created by the original Kaffeine 68000 smoke test.',10
        dc.b    'This file was closed, reopened and checked byte for byte.',10
payload_end:
payload_len equ payload_end-payload
        even

        section bss,bss
readback:
        ds.b    payload_len+1
        even
