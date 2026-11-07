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
