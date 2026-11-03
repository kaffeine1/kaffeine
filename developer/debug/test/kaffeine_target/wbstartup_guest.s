; Copyright (C) 2026, The AROS Development Team. All rights reserved.
; Author: Michele Dipace (@kaffeine1)
;
; Original 68000 Workbench startup probe. No SDK headers or C runtime.
; The native companion supplies three arguments and checks the real reply.
        section code,code
start:
        movem.l d2-d7/a2-a6,-(sp)
        moveq   #0,d7
        suba.l  a5,a5
        move.l  4.w,a6
        move.l  a6,a4
        suba.l  a1,a1
        jsr     -294(a6)               ; FindTask(NULL)
        move.l  d0,a2
        tst.l   172(a2)                ; pr_CLI
        bne     finish
        bset    #0,d7
        lea     92(a2),a2              ; pr_MsgPort
        move.l  a2,a0
        jsr     -384(a6)               ; WaitPort
        move.l  a2,a0
        jsr     -372(a6)               ; GetMsg
        move.l  d0,a3
        tst.l   d0
        beq     finish
        cmp.b   #5,8(a3)
        bne.s   .args
        cmp.w   #40,18(a3)
        bne.s   .args
        cmpa.l  20(a3),a2              ; sm_Process is the guest port
        bne.s   .args
        tst.l   32(a3)                 ; no ToolWindow in this fixture
        bne.s   .args
        bset    #1,d7
.args:
        cmp.l   #3,28(a3)
        bne     report
        move.l  36(a3),a2
        move.l  a2,d0
        beq     report
        bset    #2,d7
        move.l  4(a2),a0
        lea     program(pc),a1
        bsr     equal
        tst.l   d0
        beq.s   .segment
        move.l  12(a2),a0
        lea     first(pc),a1
        bsr     equal
        tst.l   d0
        beq.s   .segment
        move.l  20(a2),a0
        lea     second(pc),a1
        bsr     equal
        tst.l   d0
        beq.s   .segment
        bset    #3,d7
.segment:
        move.l  24(a3),d0              ; real guest seglist BPTR
        beq.s   .dos
        lsl.l   #2,d0
        addq.l  #4,d0
        lea     start(pc),a0
        cmp.l   a0,d0
        bne.s   .dos
        bset    #4,d7
.dos:
        move.l  a4,a6
        lea     dosname(pc),a1
        moveq   #0,d0
        jsr     -552(a6)
        move.l  d0,a5
        tst.l   d0
        beq     reply
        move.l  a5,a6
        lea     alpha(pc),a1
        lea     8(a2),a0
        bsr     readarg
        tst.l   d0
        beq.s   .second
        bset    #5,d7
.second:
        lea     beta(pc),a1
        lea     16(a2),a0
        bsr     readarg
        tst.l   d0
        beq.s   report
        bset    #6,d7
report:
        move.l  a5,d0
        beq.s   reply
        lea     result(pc),a0
        move.l  d7,4(a0)
        move.l  28(a3),8(a0)
        lea     path(pc),a0
        move.l  a0,d1
        move.l  #1006,d2
        move.l  a5,a6
        jsr     -30(a6)                ; Open MODE_NEWFILE
        move.l  d0,d6
        beq.s   .closebase
        move.l  d6,d1
        lea     result(pc),a0
        move.l  a0,d2
        moveq   #12,d3
        jsr     -48(a6)                ; Write
        cmp.l   #12,d0
        beq.s   .closefile
        moveq   #0,d7
.closefile:
        move.l  d6,d1
        jsr     -36(a6)
.closebase:
        move.l  a5,a1
        move.l  a4,a6
        jsr     -414(a6)
reply:
        move.l  a3,a1
        move.l  a4,a6
        jsr     -378(a6)               ; ReplyMsg, real native reply required
finish:
        moveq   #20,d0
        cmp.l   #127,d7
        bne.s   .return
        moveq   #0,d0
.return:
        movem.l (sp)+,d2-d7/a2-a6
        rts

; Compare bounded, known fixture strings, including their final NUL.
equal:
        move.l  a0,d0
        beq.s   .done
        moveq   #31,d1
.loop:
        move.b  (a0)+,d0
        cmp.b   (a1)+,d0
        bne.s   .bad
        tst.b   d0
        beq.s   .yes
        dbra    d1,.loop
.bad:
        moveq   #0,d0
        rts
.yes:
        moveq   #1,d0
.done:
        rts

; A0 is a classic WBArg, A1 the four expected bytes. Restore CurrentDir.
readarg:
        movem.l d2-d6/a0-a1,-(sp)
        move.l  (a0),d1
        beq.s   .bad
        move.l  a0,d4
        move.l  (a1),d5
        jsr     -126(a6)               ; CurrentDir
        move.l  d0,d6
        move.l  d4,a0
        move.l  4(a0),d1
        move.l  #1005,d2
        jsr     -30(a6)
        move.l  d0,d4
        beq.s   .restorebad
        move.l  d4,d1
        lea     buffer(pc),a0
        move.l  a0,d2
        moveq   #4,d3
        jsr     -42(a6)
        move.l  d0,d3
        move.l  d4,d1
        jsr     -36(a6)
        move.l  d6,d1
        jsr     -126(a6)
        cmp.l   #4,d3
        bne.s   .bad
        lea     buffer(pc),a0
        cmp.l   (a0),d5
        bne.s   .bad
        moveq   #1,d0
        bra.s   .done
.restorebad:
        move.l  d6,d1
        jsr     -126(a6)
.bad:
        moveq   #0,d0
.done:
        movem.l (sp)+,d2-d6/a0-a1
        rts

program: dc.b "wbstartup_guest",0
first:   dc.b "first",0
second:  dc.b "second",0
dosname: dc.b "dos.library",0
path:    dc.b "SYS:EMU68/wbstartup-result.bin",0
        even
alpha:   dc.b "ALFA"
beta:    dc.b "BETA"
buffer:  ds.b 4
result:  dc.b "WBS1"
        dc.l 0,0
