| XSP fast paths (see src/xsp.c).
|
| During the frame the original code calls (register arguments, so every
| register the originals keep is kept):
|
|   h_xsp_set     d0 x, d1 y, d2 PCG, d3 info  -> d0 = 0 if off screen
|   h_xsp_set_obj a0 -> {x, y, PCG, info}
|   h_xobj_set    d0 x, d1 y, d2 ref, d3 info
|
| x, y are 1/64 pixel XSP coordinates; info: bit 15 V flip, 14 H flip,
| 11-8 palette, 5-0 priority (larger = front).  Like XSP, the calls are only
| recorded (8 bytes each, one list per priority); xsp_build() then expands
| them front to back into hardware sprites and stops at the 64-sprite limit,
| so hidden sprites cost almost nothing.
|
| Call record: x, y, id, info.  id: bit 15 = single PCG (bits 0-14), else a
| ref number; id $C000 | tile = 8x8 text cell (x, y in pixels).

    .include "spr_def.inc"
    .set    MAX_CALLS, 255
    .set    MAX_HW, 64
    .set    PLAYER_LO, 0xABD                | player.ref, player2.ref
    .set    PLAYER_HI, 0xAD5
    .set    EFFECT_LO, 0x0B5                | expl2 .. expl4
    .set    EFFECT_HI, 0x210
    .set    PARTICLE_LO, 0x210              | spark, debris1, debris2: not drawn
    .set    PARTICLE_HI, 0xAA8
    .set    EFFECT_PR, 0x31                 | effects: below the enemies ($34)
    .set    EXPL4_LO, 0x0C0                 | expl4 (the enemy explosions, one sprite per
    .set    EXPL4_HI, 0x210                 | PCG): all PCGs while they fit the explosion's
                                            | share of FX_MAX, else every other one
    .set    FX_MAX, 28                      | at most this many effect sprites
    .set    DEBRIS_LO, 0x0B4                | debris.sp (particles): not drawn
    .set    DEBRIS_HI, 0x104

    .text
    .globl  h_xsp_set, h_xsp_set_obj, h_xobj_set, xsp_call, xsp_build

| ---- record a call: d0 x, d1 y, d2 id, d3 info.  Clobbers d1/a0/a1.
record:
    addq.l  #1, stat_rec
    move.l  %d4, -(%sp)
    move.w  ncalls, %d4
    cmp.w   #MAX_CALLS, %d4
    bcc.s   9f
    move.w  %d4, %a1
    lsl.w   #3, %d4
    lea     calls, %a0
    add.w   %d4, %a0
    move.w  %d0, (%a0)+
    move.w  %d1, (%a0)+
    move.w  %d2, (%a0)+
    move.w  %d3, (%a0)
    move.w  %a1, %d4                    | call index
    move.w  rec_pr, %d1                 | priority (set by the caller)
    lea     bhead, %a1
    lea     call_next, %a0
    move.b  0(%a1,%d1.w), 0(%a0,%d4.w)
    move.b  %d4, 0(%a1,%d1.w)
    addq.w  #1, ncalls
    cmp.w   pr_lo, %d1
    bcc.s   1f
    move.w  %d1, pr_lo
1:  cmp.w   pr_hi, %d1
    bls.s   9f
    move.w  %d1, pr_hi
9:  move.l  (%sp)+, %d4
    rts

h_xsp_set_obj:
    movem.l %d1-%d3, -(%sp)
    movem.w (%a0), %d0-%d3
    bsr.s   h_xsp_set
    movem.l (%sp)+, %d1-%d3
    rts

| xsp_set (the original clobbers d0/a0)
h_xsp_set:
    cmp.w   #0x4400, %d0
    bcc.s   9f
    cmp.w   #0x4400, %d1
    bcc.s   9f
    cmp.w   #NPCG, %d2
    bcc.s   8f
    movem.l %d1-%d2/%a1, -(%sp)
    move.w  %d3, %d1
    and.w   #63, %d1
    cmp.w   #DEBRIS_LO, %d2             | debris.sp PCGs (particles): not drawn
    bcs.s   1f
    cmp.w   #DEBRIS_HI, %d2
    bcs.s   7f
1:  move.w  %d1, rec_pr
    move.l  (%sp), %d1
    or.w    #0x8000, %d2
    bsr     record
7:  movem.l (%sp)+, %d1-%d2/%a1
8:  moveq   #1, %d0
    rts
9:  moveq   #0, %d0
    rts

| xobj_set (the original clobbers d0-d4/a0-a2)
| Sprite budget (64 per frame): the player ship is drawn before everything
| but the text and the bomb ($3F; the priorities in use stay within $31-$3F
| so the list scan in xsp_build stays short);
| explosions (refs EFFECT_LO..EFFECT_HI) behind everything else and FX_MAX
| sprites at most, shared evenly; an expl4 frame larger than its share is
| drawn with every other PCG (every explosion is shown, the original look
| while only a few are on screen); particles (sparks, debris,
| debris.sp PCGs) are not drawn at all,
| so they no longer push the player, enemies and bullets out of the sprite
| table (or eat the frame time).
h_xobj_set:
    cmp.w   #NREF, %d2
    bcc.s   9f
    move.w  %d3, %d4                    | (the original clobbers d0-d4)
    and.w   #63, %d4
    cmp.w   #PLAYER_LO, %d2
    bcs.s   2f
    cmp.w   #PLAYER_HI, %d2
    bcc.s   2f
    moveq   #0x3E, %d4                  | player: in front of enemies, bullets ($3C)
    bra.s   8f
2:  cmp.w   #EFFECT_LO, %d2
    bcs.s   8f
    cmp.w   #PARTICLE_HI, %d2
    bcc.s   8f
    cmp.w   #PARTICLE_LO, %d2
    bcc.s   9f                          | particles are not drawn
    cmp.w   #EFFECT_PR, %d4
    bls.s   8f
    moveq   #EFFECT_PR, %d4
8:  move.w  %d4, rec_pr
    bra     record
9:  rts

| void xsp_call(s16 x, s16 y, u16 id, u16 info) for C (text cells)
xsp_call:
    movem.l %d2-%d3, -(%sp)
    move.l  12(%sp), %d0
    move.l  16(%sp), %d1
    move.l  20(%sp), %d2
    move.l  24(%sp), %d3
    move.w  #0x3F, rec_pr               | text: the very front (recorded last)
    bsr     record
    movem.l (%sp)+, %d2-%d3
    rts

| ---- u16 xsp_build(HwSpr *table): expand the recorded calls, front first.
| Registers: a0 table write pointer, a2 = xframe, a4 = htab, d7 sprites
| written; stack: priority, call index, (refs) pieces left, object palette.
| Pieces are only roughly clipped (32 px margin): the VDP clips the rest.
xsp_build:
    movem.l %d2-%d7/%a2-%a6, -(%sp)
    move.l  48(%sp), %a0
    move.w  xframe, %a2
    lea     htab, %a4
    moveq   #0, %d7
    move.w  pr_hi, %d0
    move.w  %d0, -(%sp)                 | (sp) = current priority
1:  move.w  (%sp), %d0
    cmp.w   pr_lo, %d0
    blt     90f
    move.w  #MAX_HW, hw_limit
    move.w  #0x7FFF, fx_cap
    cmp.w   #EFFECT_PR, %d0
    bne.s   3f
    move.w  %d7, %d1                    | effects: FX_MAX more at most
    add.w   #FX_MAX, %d1
    cmp.w   #MAX_HW, %d1
    bcc.s   4f
    move.w  %d1, hw_limit
4:  lea     bhead, %a1                  | shared fairly: every explosion gets FX_MAX /
    moveq   #0, %d1                     | count sprites (2 * that many pieces, half drawn)
    move.b  0(%a1,%d0.w), %d1
    lea     call_next, %a1
    moveq   #0, %d2
5:  cmp.b   #0xFF, %d1
    beq.s   6f
    addq.w  #1, %d2
    move.b  0(%a1,%d1.w), %d1
    bra.s   5b
6:  tst.w   %d2
    beq.s   3f
    move.l  #2 * FX_MAX, %d1
    divu    %d2, %d1
    cmp.w   #2, %d1
    bcc.s   7f
    moveq   #2, %d1
7:  move.w  %d1, fx_cap
3:  lea     bhead, %a1
    moveq   #0, %d6
    move.b  0(%a1,%d0.w), %d6
    st      0(%a1,%d0.w)
2:  cmp.b   #0xFF, %d6
    beq     80f
    cmp.w   hw_limit, %d7
    bcc     80f
    move.w  %d6, -(%sp)                 | keep the call index
    lsl.w   #3, %d6
    lea     calls, %a3
    add.w   %d6, %a3
    move.w  4(%a3), %d2                 | id
    bpl     30f                         | ref
    btst    #14, %d2
    bne     20f

| -- single PCG
    move.w  (%a3)+, %d0
    asr.w   #6, %d0
    sub.w   #16, %d0                    | sx
    move.w  (%a3)+, %d1
    asr.w   #6, %d1
    sub.w   #16, %d1                    | sy
    move.w  %d0, %d4
    add.w   #15, %d4
    cmp.w   #270, %d4                   | -15 <= sx <= 255
    bhi     70f
    move.w  %d1, %d4
    add.w   #15, %d4
    cmp.w   #238, %d4                   | -15 <= sy <= 223
    bhi     70f
    and.w   #0x7FFF, %d2
    add.w   %d2, %d2
    lea     pcg_block, %a1
    move.w  0(%a1,%d2.w), %d4
    move.w  2(%a3), %d3                 | info
    move.w  %d3, %d2
    lsr.w   #8, %d2
    and.w   #15, %d2                    | palette
    lsl.w   #4, %d4
    or.w    %d2, %d4                    | key
    move.w  %d4, %d5
    lsr.w   #7, %d5
    eor.w   %d4, %d5
    and.w   #511, %d5
    lsl.w   #3, %d5
    cmp.w   0(%a4,%d5.w), %d4
    bne.s   12f
    move.w  %a2, 4(%a4,%d5.w)
    move.w  2(%a4,%d5.w), %d4
11: add.w   %d2, %d2
    lea     pal_attr, %a1
    or.w    0(%a1,%d2.w), %d4
    and.w   #0xC000, %d3
    lsr.w   #3, %d3                     | flips -> attr bits 11/12
    or.w    %d3, %d4
    add.w   #128, %d1
    move.w  %d1, (%a0)+
    addq.w  #1, %d7
    move.w  %d7, %d3
    or.w    #0x0500, %d3                | 2x2 cells | link
    move.w  %d3, (%a0)+
    move.w  %d4, (%a0)+
    add.w   #128, %d0
    move.w  %d0, (%a0)+
    bra     70f
12: bsr     lookup_slow
    bne.s   11b
    bra     70f

| -- text cell (x, y in pixels)
20: move.w  2(%a3), %d1
    add.w   #128, %d1
    move.w  %d1, (%a0)+
    addq.w  #1, %d7
    move.w  %d7, (%a0)+                 | 1x1 | link
    and.w   #0x07FF, %d2
    or.w    #0x8000 + (TEXT_PAL << 13), %d2
    move.w  %d2, (%a0)+
    move.w  (%a3), %d0
    add.w   #128, %d0
    move.w  %d0, (%a0)+
    bra     70f

| -- composite ref
30: move.w  (%a3), %d0
    asr.w   #6, %d0
    move.w  %d0, %a5                    | px
    move.w  2(%a3), %d0
    asr.w   #6, %d0
    move.w  %d0, %a6                    | py
    move.w  6(%a3), %d3                 | info
    move.w  %d3, %d0
    lsr.w   #8, %d0
    and.w   #15, %d0
    move.w  %d0, -(%sp)                 | 2(sp) = object palette (after the next push)
    move.w  %d3, %d6
    rol.w   #2, %d6
    and.w   #3, %d6
    lsl.w   #4, %d6                     | object flips at bits 4 (H) / 5 (V)
    cmp.w   #EXPL4_LO, %d2
    bcs.s   29f
    cmp.w   #EXPL4_HI, %d2
    bcc.s   29f
    or.w    #0x100, %d6                 | bit 8: draw every other piece (if needed)
29:
    add.w   %d2, %d2
    lea     ref_start, %a1
    moveq   #0, %d0
    move.w  0(%a1,%d2.w), %d0
    move.w  2(%a1,%d2.w), %d1
    sub.w   %d0, %d1
    beq     69f
    btst    #8, %d6
    beq.s   27f
    move.w  fx_cap, %d5                 | share = fx_cap / 2 sprites
    lsr.w   #1, %d5
    cmp.w   %d5, %d1
    bhi.s   27f
    bclr    #8, %d6                     | fits: every piece
27: cmp.w   fx_cap, %d1
    bls.s   28f
    move.w  fx_cap, %d1
28: subq.w  #1, %d1
    move.w  %d1, -(%sp)                 | (sp) = pieces left - 1
    lsl.l   #3, %d0
    lea     ref_pieces, %a3
    add.l   %d0, %a3
31: btst    #8, %d6
    beq.s   39f
    btst    #0, 1(%sp)                  | odd pieces-left count: skipped
    beq.s   39f
    addq.l  #8, %a3
    bra     68f
39: move.w  (%a3)+, %d0                 | dx
    move.w  (%a3)+, %d1                 | dy
    moveq   #0, %d2
    move.b  (%a3)+, %d2                 | size | flips << 4
    moveq   #0, %d3
    move.b  (%a3)+, %d3                 | palette xor
    move.w  (%a3)+, %d4                 | block
    tst.w   %d6
    beq.s   34f
    move.w  %d2, %d5
    and.w   #15, %d5
    add.w   %d5, %d5                    | size index (words)
    btst    #4, %d6                     | object H flip: mirror around the centre
    beq.s   32f
    neg.w   %d0
    sub.w   #16, %d0
    lea     size_w, %a1
    sub.w   0(%a1,%d5.w), %d0
32: btst    #5, %d6
    beq.s   33f
    neg.w   %d1
    sub.w   #16, %d1
    lea     size_h, %a1
    sub.w   0(%a1,%d5.w), %d1
33: eor.w   %d6, %d2
34: add.w   %a5, %d0                    | sx
    move.w  %d0, %d5
    add.w   #32, %d5
    cmp.w   #288, %d5                   | -32 < sx < 256
    bcc.s   68f
    add.w   %a6, %d1                    | sy
    move.w  %d1, %d5
    add.w   #32, %d5
    cmp.w   #256, %d5                   | -32 < sy < 224
    bcc.s   68f
    move.w  2(%sp), %d5
    eor.w   %d5, %d3
    and.w   #15, %d3                    | palette
    lsl.w   #4, %d4
    or.w    %d3, %d4                    | key
    move.w  %d4, %d5
    lsr.w   #7, %d5
    eor.w   %d4, %d5
    and.w   #511, %d5
    lsl.w   #3, %d5
    cmp.w   0(%a4,%d5.w), %d4
    bne.s   38f
    move.w  %a2, 4(%a4,%d5.w)
    move.w  2(%a4,%d5.w), %d4
37: add.w   %d3, %d3
    lea     pal_attr, %a1
    or.w    0(%a1,%d3.w), %d4
    move.w  %d2, %d5
    and.w   #0x30, %d5
    lsl.w   #7, %d5                     | flips -> attr bits 11/12
    or.w    %d5, %d4
    and.w   #15, %d2
    lsl.w   #8, %d2
    add.w   #128, %d1
    move.w  %d1, (%a0)+
    addq.w  #1, %d7
    or.w    %d7, %d2                    | size | link
    move.w  %d2, (%a0)+
    move.w  %d4, (%a0)+
    add.w   #128, %d0
    move.w  %d0, (%a0)+
    cmp.w   hw_limit, %d7
    bcc.s   67f
68: subq.w  #1, (%sp)
    bpl     31b
67: addq.l  #2, %sp
69: addq.l  #2, %sp
    bra.s   70f
38: bsr.s   lookup_slow
    bne.s   37b
    bra.s   68b

| -- next call in this priority
70: move.w  (%sp)+, %d6
    lea     call_next, %a1
    move.b  0(%a1,%d6.w), %d6
    bra     2b
80: subq.w  #1, (%sp)
    bra     1b
90: addq.l  #2, %sp
    tst.w   %d7
    beq.s   91f
    clr.b   -5(%a0)                     | end of the link chain
91: add.l   %d7, stat_out
    move.w  %d7, %d0
    movem.l (%sp)+, %d2-%d7/%a2-%a6
    rts

| key d4 not at its home slot: probe on (d5 = home offset); d4 = tile, Z if none
lookup_slow:
1:  cmpi.w  #-1, 0(%a4,%d5.w)
    beq.s   4f
    addq.w  #8, %d5
    and.w   #0xFFF, %d5
    cmp.w   0(%a4,%d5.w), %d4
    bne.s   1b
    move.w  %a2, 4(%a4,%d5.w)
    move.w  2(%a4,%d5.w), %d4
    rts
4:  movem.l %d0-%d3/%a0-%a1, -(%sp)     | C clobbers d0/d1/a0/a1
    move.l  %d4, -(%sp)
    jsr     cache_miss
    addq.l  #4, %sp
    move.w  %d0, %d4
    movem.l (%sp)+, %d0-%d3/%a0-%a1
    tst.w   %d4
    rts

    .section .note.GNU-stack,"",%progbits

    .bss
    .globl  stat_pieces, stat_calls, stat_out, stat_rec
rec_pr:      .word 0
hw_limit:    .word 0
fx_cap:      .word 0
    .balign 2
stat_pieces: .long 0
stat_calls:  .long 0
stat_out:    .long 0
stat_rec:    .long 0
