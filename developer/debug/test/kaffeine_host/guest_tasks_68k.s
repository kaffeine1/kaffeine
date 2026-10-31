; Copyright (C) 2026, The AROS Development Team. All rights reserved.
; Author: Michele Dipace (@kaffeine1)
;
; Original parent and child of test_guest_tasks.cpp, which holds them
; assembled (moira_parent, moira_child) and runs them on real Moira CPUs.
; The test places the parent at $11000, the child at $12000 and the child's
; Task, whose name, priority and stack bounds it prepares, at $20000.
; Results go to $A000-$A01B. Signals: CTRL_F (bit 15) to the parent,
; CTRL_E (bit 14) to the child, both classic system signals.

_LVOAddTask     equ     -282
_LVOFindTask    equ     -294
_LVOWait        equ     -318
_LVOSignal      equ     -324

CHILD_TASK      equ     $20000
ADDED           equ     $A000           ; AddTask() result
PARENT_WOKE     equ     $A004           ; the parent's first Wait() result
PARENT_DONE     equ     $A008           ; the parent's second Wait() result
CHILD_VALUE     equ     $A00C           ; what the child computed
PARENT_TASK     equ     $A010           ; FindTask(NULL) of the parent
CHILD_SELF      equ     $A014           ; FindTask(NULL) of the child
CHILD_WOKE      equ     $A018           ; the child's Wait() result

; Entered as a program: A6 = SysBase, the sentinel return address on SP.
parent:
        suba.l  a1,a1
        jsr     _LVOFindTask(a6)
        move.l  d0,PARENT_TASK
        lea     CHILD_TASK,a1
        lea     child,a2                ; $12000 in the test
        suba.l  a3,a3                   ; default finalPC
        jsr     _LVOAddTask(a6)
        move.l  d0,ADDED
        move.l  #$8000,d0
        jsr     _LVOWait(a6)            ; the child runs meanwhile
        move.l  d0,PARENT_WOKE
        lea     CHILD_TASK,a1
        move.l  #$4000,d0
        jsr     _LVOSignal(a6)
        move.l  #$8000,d0
        jsr     _LVOWait(a6)
        move.l  d0,PARENT_DONE
        move.l  CHILD_VALUE,d0          ; the program's return code
        rts

; Entered by AddTask() with every register 0 but SP.
child:
        movea.l 4.w,a6
        suba.l  a1,a1
        jsr     _LVOFindTask(a6)
        move.l  d0,CHILD_SELF
        moveq   #42,d1
        move.l  d1,CHILD_VALUE
        movea.l PARENT_TASK,a1
        move.l  #$8000,d0
        jsr     _LVOSignal(a6)
        move.l  #$4000,d0
        jsr     _LVOWait(a6)
        move.l  d0,CHILD_WOKE
        movea.l PARENT_TASK,a1
        move.l  #$8000,d0
        jsr     _LVOSignal(a6)
        rts                             ; ends the child alone
