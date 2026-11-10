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

; Keyboard reset handler. a1 = ResetState {task, mask, pip2, pip1}: turn
; both SAGA PiP units off (SAGA keeps them on through a reboot) and signal
; the main task, which answers KBD_RESETHANDLERDONE.
    xdef _dock_reset_entry
_dock_reset_entry:
    move.l 8(a1),d0
    beq.s .reset_pip1
    move.l d0,a0
    clr.w (a0)
.reset_pip1:
    move.l 12(a1),d0
    beq.s .reset_signal
    move.l d0,a0
    clr.w (a0)
.reset_signal:
    move.l a6,-(sp)
    move.l 4.w,a6
    move.l 4(a1),d0
    move.l (a1),a1
    jsr -324(a6)            ; Signal(task, mask)
    move.l (sp)+,a6
    moveq #0,d0
    rts

; Vertical-blank interrupt server. a1 = VBlankState {task, mask, armed,
; fields, flip_register, flip_buffer}: count every field, show a pending
; PiP2 buffer, and while armed signal the main task. Return
; with Z set so the rest of the VERTB chain runs.
    xdef _dock_vblank_entry
_dock_vblank_entry:
    addq.l #1,12(a1)
    move.l 20(a1),d0        ; a PiP2 buffer to show from this field
    beq.s .no_flip
    clr.l 20(a1)
    move.l 16(a1),a0
    move.l d0,(a0)
.no_flip:
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
