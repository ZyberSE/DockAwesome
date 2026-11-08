; input.device handler register ABI -> GCC stack ABI.
; a0 = InputEvent chain, a1 = application context; return chain in d0.
; The C handler may null captured button events; the chain links remain intact.
    section .text,code
    xdef _dock_input_entry
_dock_input_entry:
    movem.l d2-d7/a2-a6,-(sp)
    move.l a1,-(sp)
    move.l a0,-(sp)
    jsr _dock_input_observe
    addq.l #8,sp
    movem.l (sp)+,d2-d7/a2-a6
    rts

; Call the installed Apollo console handler using the input.device ABI.
    xdef _dock_call_console_input
_dock_call_console_input:
    movem.l d2-d7/a2-a6,-(sp)
    move.l 48(sp),a2
    move.l 52(sp),a0
    move.l 56(sp),a1
    jsr (a2)
    movem.l (sp)+,d2-d7/a2-a6
    rts

; Workbench selection hook used only to read desktop icon bounds.
    xdef _dock_workbench_hit_entry
_dock_workbench_hit_entry:
    movem.l d2-d7/a2-a6,-(sp)
    move.l a1,-(sp)
    move.l a0,-(sp)
    jsr _dock_workbench_hit
    addq.l #8,sp
    movem.l (sp)+,d2-d7/a2-a6
    rts

; Layers backfill register ABI -> GCC stack ABI.
; a0 = hook, a2 = RastPort, a1 = clipped backfill message.
    xdef _dock_backfill_entry
_dock_backfill_entry:
    movem.l d2-d7/a2-a6,-(sp)
    move.l a1,-(sp)
    move.l a2,-(sp)
    move.l a0,-(sp)
    jsr _dock_backfill_paint
    lea 12(sp),sp
    movem.l (sp)+,d2-d7/a2-a6
    rts

    xdef _dock_folder_backfill_entry
_dock_folder_backfill_entry:
    movem.l d2-d7/a2-a6,-(sp)
    move.l a1,-(sp)
    move.l a2,-(sp)
    move.l a0,-(sp)
    jsr _dock_folder_backfill
    lea 12(sp),sp
    movem.l (sp)+,d2-d7/a2-a6
    rts

; Vertical-blank interrupt server. a1 = VBlankState {task, mask, armed,
; fields}: count every field, and while armed signal the main task. Return
; with Z set so the rest of the VERTB chain runs.
    xdef _dock_vblank_entry
_dock_vblank_entry:
    addq.l #1,12(a1)
    tst.l 8(a1)
    beq.s .vblank_done
    move.l a6,-(sp)
    move.l 4.w,a6
    move.l 4(a1),d0
    move.l (a1),a1
    jsr -324(a6)            ; Signal(task, mask)
    move.l (sp)+,a6
.vblank_done:
    lea $dff000,a0          ; as the next server in the chain expects
    moveq #0,d0
    rts
