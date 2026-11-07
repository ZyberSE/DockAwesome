; DockAwesome: optional 68080 AMMX rounded 50/50 byte mixer.
; void dock_mix_half_ammx(unsigned char *dst, const unsigned char *src,
;                         unsigned long pixel_count);
;
; GCC m68k stack ABI. One pixel = four bytes, any channel ordering.
; dst may equal src. Other overlapping ranges are unsupported.
; Only D0/D1 and A0/A1 are volatile; D2/D3 are saved. No E registers,
; framebuffer registers, OS calls, or speculative reads beyond the range.
; PAVGB performs eight independent (src + dst + 1) >> 1 operations.

    section .text,code
    xdef _dock_mix_half_ammx
    cnop 0,4
_dock_mix_half_ammx:
    movem.l d2-d3,-(sp)
    move.l 12(sp),a0
    move.l 16(sp),a1
    move.l 20(sp),d2
    move.l d2,d3
    and.l #1,d3
    lsr.l #1,d2
    beq.s .tail
.pairs:
    load (a1)+,d0
    pavgb (a0),d0,d1
    store d1,(a0)+
    subq.l #1,d2
    bne.s .pairs
.tail:
    tst.l d3
    beq.s .done
    moveq #3,d2
.byte:
    moveq #0,d0
    moveq #0,d1
    move.b (a0),d0
    move.b (a1)+,d1
    add.w d1,d0
    addq.w #1,d0
    lsr.w #1,d0
    move.b d0,(a0)+
    dbra d2,.byte
.done:
    movem.l (sp)+,d2-d3
    rts

; void dock_rgba_to_rgb24_ammx(unsigned char *dst,
;                            const unsigned char *src, unsigned long pixels);
; Four RGBA pixels -> twelve packed RGB bytes; ignore alpha.
; No reads/stores beyond the specified spans. Partial overlap unsupported.
    xdef _dock_rgba_to_rgb24_ammx
    cnop 0,4
_dock_rgba_to_rgb24_ammx:
    movem.l d2-d3,-(sp)
    move.l 12(sp),a0
    move.l 16(sp),a1
    move.l 20(sp),d2
    move.l d2,d3
    and.l #3,d3
    lsr.l #2,d2
    beq.s .rgb_tail
.rgb_quads:
    load (a1)+,d0
    load (a1)+,d1
    vperm #$01245689,d0,d1,d0
    store d0,(a0)+
    vperm #$00002456,d1,d1,d0
    move.l d0,(a0)+
    subq.l #1,d2
    bne.s .rgb_quads
.rgb_tail:
    tst.l d3
    beq.s .rgb_done
.rgb_pixel:
    move.b (a1)+,(a0)+
    move.b (a1)+,(a0)+
    move.b (a1)+,(a0)+
    addq.l #1,a1
    subq.l #1,d3
    bne.s .rgb_pixel
.rgb_done:
    movem.l (sp)+,d2-d3
    rts

; BE RGB565 -> straight RGBA, replicated low color bits and alpha 255.
; Four pixels per UNPACK1632. D4's ENTIRE vector is preserved, unlike a
; scalar movem save; D2/D3 are only touched by scalar 32-bit instructions.
    xdef _dock_rgb565_to_rgba_ammx
    cnop 0,4
_dock_rgb565_to_rgba_ammx:
    movem.l d2-d3,-(sp)
    subq.l #8,sp
    store d4,(sp)
    move.l 20(sp),a0
    move.l 24(sp),a1
    move.l 28(sp),d2
    move.l d2,d3
    and.l #3,d3
    lsr.l #2,d2
    beq.s .unpack_tail
    load #$000000ff000000ff,d4
.unpack_quads:
    unpack1632 (a1)+,d0:d1
    vperm #$12305674,d0,d0,d0
    vperm #$12305674,d1,d1,d1
    por d4,d0,d0
    por d4,d1,d1
    store d0,(a0)+
    store d1,(a0)+
    subq.l #1,d2
    bne.s .unpack_quads
.unpack_tail:
    tst.l d3
    beq.s .unpack_done
.unpack_pixel:
    moveq #0,d0
    move.w (a1)+,d0
    move.l d0,d1
    lsr.w #8,d1
    and.w #$f8,d1
    move.b d1,(a0)
    lsr.b #5,d1
    or.b d1,(a0)+
    move.l d0,d1
    lsr.w #3,d1
    and.w #$fc,d1
    move.b d1,(a0)
    lsr.b #6,d1
    or.b d1,(a0)+
    move.b d0,d1
    lsl.b #3,d1
    move.b d1,(a0)
    lsr.b #2,d0
    and.b #7,d0
    or.b d0,(a0)+
    move.b #$ff,(a0)+
    subq.l #1,d3
    bne.s .unpack_pixel
.unpack_done:
    load (sp),d4
    addq.l #8,sp
    movem.l (sp)+,d2-d3
    rts

; Straight RGBA -> BE RGB565, discard alpha and truncate RGB channels.
; Source/destination spans may be unaligned; partial overlap unsupported.
    xdef _dock_rgba_to_rgb565_ammx
    cnop 0,4
_dock_rgba_to_rgb565_ammx:
    movem.l d2-d3,-(sp)
    move.l 12(sp),a0
    move.l 16(sp),a1
    move.l 20(sp),d2
    move.l d2,d3
    and.l #3,d3
    lsr.l #2,d2
    beq.s .pack_tail
.pack_quads:
    load (a1)+,d0
    load (a1)+,d1
    vperm #$30127456,d0,d0,d0
    vperm #$30127456,d1,d1,d1
    pack3216 d0,d1,(a0)+
    subq.l #1,d2
    bne.s .pack_quads
.pack_tail:
    tst.l d3
    beq.s .pack_done
.pack_pixel:
    moveq #0,d0
    move.b (a1)+,d0
    lsl.w #8,d0
    and.w #$f800,d0
    moveq #0,d1
    move.b (a1)+,d1
    lsl.w #3,d1
    and.w #$07e0,d1
    or.w d1,d0
    moveq #0,d1
    move.b (a1)+,d1
    lsr.w #3,d1
    or.w d1,d0
    addq.l #1,a1
    move.w d0,(a0)+
    subq.l #1,d3
    bne.s .pack_pixel
.pack_done:
    movem.l (sp)+,d2-d3
    rts

; int dock_bytes_equal_ammx(const unsigned char *a,
;                          const unsigned char *b, unsigned long bytes);
; Compare 32-byte blocks with 64-bit XOR/OR reduction. Only volatile D0/D1
; are vector destinations. No speculative reads beyond the byte count.
    xdef _dock_bytes_equal_ammx
    cnop 0,4
_dock_bytes_equal_ammx:
    movem.l d2-d3,-(sp)
    move.l 12(sp),a0
    move.l 16(sp),a1
    move.l 20(sp),d2
    move.l d2,d3
    and.l #31,d3
    lsr.l #5,d2
    beq.s .equal_octets
.equal_blocks:
    load (a0)+,d0
    peor (a1)+,d0,d0
    load (a0)+,d1
    peor (a1)+,d1,d1
    por d1,d0,d0
    load (a0)+,d1
    peor (a1)+,d1,d1
    por d1,d0,d0
    load (a0)+,d1
    peor (a1)+,d1,d1
    por d1,d0,d0
    tst.l d0
    bne.s .not_equal
    vperm #$00000123,d0,d0,d1
    tst.l d1
    bne.s .not_equal
    subq.l #1,d2
    bne.s .equal_blocks
.equal_octets:
    cmp.l #8,d3
    bcs.s .equal_tail
    load (a0)+,d0
    peor (a1)+,d0,d0
    tst.l d0
    bne.s .not_equal
    vperm #$00000123,d0,d0,d1
    tst.l d1
    bne.s .not_equal
    subq.l #8,d3
    bra.s .equal_octets
.equal_tail:
    tst.l d3
    beq.s .equal_yes
.equal_byte:
    cmpm.b (a0)+,(a1)+
    bne.s .not_equal
    subq.l #1,d3
    bne.s .equal_byte
.equal_yes:
    moveq #1,d0
    bra.s .equal_done
.not_equal:
    moveq #0,d0
.equal_done:
    movem.l (sp)+,d2-d3
    rts

; Key pixels outside the Dock's coverage through to live scanout.
; void dock_rgb565_key_ammx(unsigned char *dst565,
;                          const unsigned char *coverage, unsigned long pixels);
; One coverage byte/pixel: zero is transparent, every nonzero value is covered.
; Covered genuine $f81f artwork escapes to $f81e. Coverage/output are disjoint.
; D4-D7 are full 64-bit vectors, so save/restore their upper halves too.
    xdef _dock_rgb565_key_ammx
    cnop 0,4
_dock_rgb565_key_ammx:
    movem.l d2-d3,-(sp)
    sub.l #32,sp
    store d4,(sp)
    store d5,8(sp)
    store d6,16(sp)
    store d7,24(sp)
    move.l 44(sp),a0
    move.l 48(sp),a1
    move.l 52(sp),d2
    move.l d2,d3
    and.l #7,d3
    lsr.l #3,d2
    beq.s .key_tail
    load #$f81ff81ff81ff81f,d4
    load #$0001000100010001,d5
.key_octets:
    load (a1)+,d1
    vperm #$00112233,d1,d1,d6
    vperm #$44556677,d1,d1,d7
    pcmpeqw #$0000000000000000,d6,d6
    pcmpeqw #$0000000000000000,d7,d7
    load (a0),d0
    pcmpeqw d4,d0,d1
    pand d5,d1,d1
    peor d1,d0,d0
    bsel d4,d6,d0
    store d0,(a0)+
    load (a0),d0
    pcmpeqw d4,d0,d1
    pand d5,d1,d1
    peor d1,d0,d0
    bsel d4,d7,d0
    store d0,(a0)+
    subq.l #1,d2
    bne.s .key_octets
.key_tail:
    tst.l d3
    beq.s .key_done
.key_pixel:
    tst.b (a1)+
    beq.s .key_equal
    cmp.w #$f81f,(a0)
    bne.s .key_next
    move.w #$f81e,(a0)
    bra.s .key_next
.key_equal:
    move.w #$f81f,(a0)
.key_next:
    addq.l #2,a0
    subq.l #1,d3
    bne.s .key_pixel
.key_done:
    load (sp),d4
    load 8(sp),d5
    load 16(sp),d6
    load 24(sp),d7
    add.l #32,sp
    movem.l (sp)+,d2-d3
    rts
