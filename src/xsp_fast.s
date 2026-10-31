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
    .set    PLAYER_ROOM, 4                  | sprites kept free for the player after $3F
    .set    ENEMY_PR, 0x34                  | the enemies
    .set    BEHIND_PR, 0x33                 | behind a high priority plane A boss (plane_behind)
    .set    VLASER_A, 0x50                  | vertical laser segments (thick, thin): pairs
    .set    VLASER_B, 0x59                  | one above the other become one ref
    .set    NEEDLE_PCG, 0x86                | (tools/convert_sprites.py NEEDLE_PCG)
    .set    TURRET_ROOM, 24                 | and the stage 5 battleship's turrets
    .set    ENEMY_ROOM, 16                  | sprites the enemy bullets leave them at most
    .set    EFFECT_LO, 0x0B5                | expl2 .. expl4
    .set    EFFECT_HI, 0x210
    .set    PARTICLE_LO, 0x210              | spark, debris1, debris2: not drawn
    .set    PARTICLE_HI, 0xAA8
    .set    EFFECT_PR, 0x31                 | effects: below the enemies ($34)
    .set    EXPL4_LO, 0x0C0                 | expl4 (the enemy explosions, one sprite per
    .set    EXPL4_HI, 0x210                 | PCG): all PCGs while they fit the explosion's
                                            | share of FX_MAX, else every other one
    .set    ARC_LO, 0x087                   | ele_arc (the bomb's lightning, one sprite per
    .set    ARC_HI, 0x0B5                   | PCG): every other PCG, alternately by frame
    .set    PL_MAX, 64                      | pl_ix entries (src/plane.c)
    .set    FX_MAX, 28                      | at most this many effect sprites
    .set    LASER_LO, 0x50                  | laser segment PCGs (16 angles, thick / thin):
    .set    LASER_HI, 0x5F                  | paired into 32x32 sprites
    .set    PSHOT_PR, 0x3A                  | the player's shots: drawn before the enemy
    .set    PSHOT_TO, 0x3D                  | bullets ($3C) when sprites run short
    .set    SMOKE_PR, 0x38                  | missile smoke (PCGs $30-$37 at $38 / $3B): behind
    .set    SMOKE_PR2, 0x3B
    .set    SMOKE_LO, 0x30                  | the enemies ($34), dropped before them
    .set    SMOKE_HI, 0x37
    .set    SMOKE_TO, 0x32
    .set    BULLET_PR, 0x3C                 | the enemy bullets: the sprite doubler's
    .set    DBL_MAX, 40                     | (at most this many bullets in the lower part
    .set    DBL_A, 96                       | of the screen reuse the slots of bullets above
    .set    VRAM_HSCROLL, 0xDC00
    .set    DBL_B, 112                      | line DBL_A: src/xsp.c xsp_upload, the H-int)
    .set    DEBRIS_LO, 0x0B4                | debris.sp (particles): not drawn
    .set    DEBRIS_HI, 0x104

    .text
    .globl  h_xsp_set, h_xsp_set_obj, h_xobj_set, xsp_call, xsp_build

| ---- record a call: d0 x, d1 y, d2 id, d3 info.  Clobbers d1/a0/a1.
record:
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
    lea     call_next, %a0              | (bhead = call_next - 128)
    move.b  -128(%a0,%d1.w), 0(%a0,%d4.w)
    move.b  %d4, -128(%a0,%d1.w)
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
    jsr     xsp_set_vec
    movem.l (%sp)+, %d1-%d3
    rts

| xsp_set (the original clobbers d0/a0): recorded here directly (record,
| inlined: the bullets make up most calls).  The hook jumps through
| xsp_set_vec (RAM): h_xsp_set_main, or h_xsp_set_boss while a boss or a mid
| boss is about (src/xsp.c xsp_pairs): their lasers paired
h_xsp_set_boss:
    cmp.w   #VLASER_A, %d2              | (vertical: merged by h_xsp_set_main)
    beq.s   h_xsp_set_main
    cmp.w   #VLASER_B, %d2
    beq.s   h_xsp_set_main
    move.w  %d2, %a0
    sub.w   #LASER_LO, %a0
    cmp.w   #LASER_HI - LASER_LO, %a0
    bls     20f                         | a laser segment: maybe a pair
h_xsp_set_main:
    cmp.w   #0x4400, %d0
    bcc     9f
    cmp.w   #0x4400, %d1
    bcc     9f
    cmp.w   #NPCG, %d2
    bcc     8f
    cmp.w   #DEBRIS_LO, %d2             | debris.sp PCGs (particles): not drawn
    bcs.s   1f
    cmp.w   #DEBRIS_HI, %d2
    bcs     8f
1:  cmp.w   #VLASER_B, %d2             | (most PCGs above: one compare)
    bhi.s   28f
    cmp.w   #VLASER_B, %d2
    beq     30f
    cmp.w   #VLASER_A, %d2
    beq     30f
    bra.s   29f
28: cmp.w   #NEEDLE_PCG, %d2            | the stage 3 boss's needle streams (palette 1):
    bne.s   29f                         | two side by side as one sprite (pair_lut row 16)
    move.l  %d0, -(%sp)
    move.w  %d3, %d0
    and.w   #0x0F00, %d0
    cmp.w   #0x0100, %d0
    movem.l (%sp)+, %d0                 | (movem: the flags kept)
    bne.s   29f
    st      needle_try
    bra     20f
29:
xsp_set_rec:
    move.l  %d4, -(%sp)                | (only d0 / a0 are free)
    move.w  ncalls, %d4
    cmp.w   #MAX_CALLS, %d4
    bcc.s   7f
    lsl.w   #3, %d4
    lea     calls, %a0
    add.w   %d4, %a0
    lsr.w   #3, %d4
    move.w  %d0, (%a0)+
    move.w  %d1, (%a0)+
    move.w  %d2, (%a0)
    bset    #7, (%a0)+                  | (a PCG, not a ref)
    addq.l  #1, %a0
    move.w  %d3, (%a0)
    moveq   #63, %d0
    and.w   %d3, %d0                    | priority list
    move.b  pr_map(%pc,%d0.w), %d0      | (remapped: the player's shots, the smoke)
    bmi.s   5f
3:  lea     call_next, %a0              | (bhead = call_next - 128)
    move.b  -128(%a0,%d0.w), 0(%a0,%d4.w)
    move.b  %d4, -128(%a0,%d0.w)
    addq.w  #1, ncalls
    cmp.w   pr_lo, %d0
    bcc.s   2f
    move.w  %d0, pr_lo
2:  cmp.w   pr_hi, %d0
    bls.s   7f
    move.w  %d0, pr_hi
7:  move.l  (%sp)+, %d4
8:  moveq   #1, %d0
    rts
9:  moveq   #0, %d0
    rts
5:  and.w   #63, %d0                    | the smoke's priorities: smoke PCGs behind
    cmp.b   #EFFECT_PR, %d0             | the enemies
    beq.s   6f
    move.w  %d2, %a0
    sub.w   #SMOKE_LO, %a0
    cmp.w   #SMOKE_HI - SMOKE_LO, %a0
    bhi.s   3b
    moveq   #SMOKE_TO, %d0
    bra.s   3b
6:  tst.b   plane_turrets               | the stage 5 battleship on plane A (last
    beq.s   3b                          | frame): its turrets (palette 6) of the
    move.w  %d3, %d0                    | effect level to $33 with the others,
    and.w   #0x0F00, %d0                | which the enemy bullets leave sprites
    cmp.w   #0x0600, %d0                | for (enemy_room)
    beq.s   4f
    moveq   #EFFECT_PR, %d0
    bra.s   3b
4:  moveq   #BEHIND_PR, %d0
    bra.s   3b
| priority -> list: the player's shots before the enemy bullets, $80 | priority:
| look at the PCG (missile smoke, turrets)
pr_map:
    .irp    p, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .byte   \p
    .endr
    .irp    p, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48
    .byte   \p
    .endr
    .byte   0x80 + EFFECT_PR                | ($31: the stage 5 battleship's turrets, below)
    .irp    p, 50,51,52,53,54,55
    .byte   \p
    .endr
    .byte   0x80 + SMOKE_PR, 57, PSHOT_TO, 0x80 + SMOKE_PR2, 60, 61, 62, 63
    .if     . - pr_map - 64                 | (`|` starts a comment here: write `+`)
    .error  "pr_map: not 64 entries"
    .endif
    .balign 2

| A laser segment (rows of bullets change their spacing every frame: no
| reuse) next to the same segment (same attributes) just recorded at this
| priority, both within 32x32: the two become one 32x32 sprite (xsp_build,
| xsp.c pair_tile): info bit 12, the offset in the first one's x / y
| fraction bits (the sprites only use whole pixels)
20: cmp.w   #0x4400, %d0               | (as h_xsp_set_main)
    bcc     9b
    cmp.w   #0x4400, %d1
    bcc     9b
    movem.l %d4-%d6/%a1, -(%sp)
    moveq   #63, %d4
    and.w   %d3, %d4                    | priority
    lea     call_next, %a0
    moveq   #0, %d5
    move.b  -128(%a0,%d4.w), %d5        | the last calls of this priority (the
    swap    %d4                         | two beams of a pair of lasers come in
    move.w  #3, %d4                     | turns): up to 4 back
4:  cmp.b   #-1, %d5
    beq     25f
    move.w  %d5, %a0                    | (its index)
    lsl.w   #3, %d5
    lea     calls, %a1
    add.w   %d5, %a1
    move.w  %d2, %d5
    bset    #15, %d5
    cmp.w   4(%a1), %d5                 | the same PCG
    bne.s   6f
    move.w  %d3, %d5
    bclr    #12, %d5
    cmp.w   6(%a1), %d5                 | the same info, not paired yet
    bne.s   6f
    move.w  (%a1), %d5
    asr.w   #6, %d5
    move.w  %d0, %d6
    asr.w   #6, %d6
    sub.w   %d5, %d6
    add.w   #16, %d6                    | dx + 16 in 0..32
    cmp.w   #32, %d6
    bhi.s   6f
    move.w  2(%a1), %d5
    asr.w   #6, %d5
    swap    %d4                         | (low word: free, the count kept in the high one)
    move.w  %d1, %d4
    asr.w   #6, %d4
    sub.w   %d5, %d4
    add.w   #16, %d4                    | dy + 16 in 0..32
    cmp.w   #32, %d4
    bls.s   3f
    swap    %d4
6:  move.w  %a0, %d5                    | next back in the list
    lea     call_next, %a0
    move.b  0(%a0,%d5.w), %d5
    and.w   #0xFF, %d5
    dbra    %d4, 4b
    bra     25f
3:  move.w  %d2, %d0                    | a pre-composited pair (pair_lut, by the
    sub.w   #LASER_LO, %d0              | step unflipped): the first call becomes
    tst.b   needle_try                  | its ref (tools/convert_sprites.py PAIR_STEP)
    beq.s   37f
    moveq   #16, %d0                    | (the needles' row)
37: move.w  %d0, %d5
    lsl.w   #5, %d5
    add.w   %d0, %d5                    | pcg * 33
    move.w  %d5, %d0
    lsl.w   #5, %d0
    add.w   %d5, %d0                    | pcg * 33 * 33
    move.w  %d4, %d5                    | dy + 16
    tst.w   %d3
    bpl.s   34f
    neg.w   %d5                         | (V flip)
    add.w   #32, %d5
34: add.w   %d5, %d0
    lsl.w   #5, %d5
    add.w   %d5, %d0                    | + (dy + 16) * 33
    move.w  %d6, %d5                    | dx + 16
    btst    #14, %d3
    beq.s   35f
    neg.w   %d5                         | (H flip)
    add.w   #32, %d5
35: add.w   %d5, %d0
    lea     pair_lut, %a0
    moveq   #0, %d5
    move.w  %d0, %d5
    add.l   %d5, %d5
    move.w  0(%a0,%d5.l), %d5
    beq.s   36f
    add.w   #PAIR_REF - 1, %d5
    move.w  %d5, 4(%a1)
    movem.l (%sp)+, %d4-%d6/%a1
    sf      needle_try
    moveq   #1, %d0
    rts
36: tst.b   needle_try                  | (needles: no runtime pair, recorded as they are)
    bne.s   25f
    bset    #6, 4(%a1)                  | id bit 14 (as a text cell: xsp_build looks
    bset    #4, 6(%a1)                  | there) and info bit 12: a pair, dx + 16 / dy + 16 in
    and.w   #0xFFC0, (%a1)              | the first one's x / y fraction bits (the
    or.w    %d6, (%a1)                  | sprites only use whole pixels)
    and.w   #0xFFC0, 2(%a1)
    or.w    %d4, 2(%a1)
    movem.l (%sp)+, %d4-%d6/%a1
    moveq   #1, %d0
    rts
25: movem.l (%sp)+, %d4-%d6/%a1
    tst.b   needle_try
    beq     h_xsp_set_main
    sf      needle_try
    bra     xsp_set_rec

| a turret's second barrel (the original's call sites: tools/relocate.py
| TURRET_PAIR2): the same PCG and info as the call just recorded (the first
| one), within 16 px: that call becomes the pair's ref (pair_lut rows
| TURRET_ROW..), this one is not recorded
    .globl  h_tur_a1, h_tur_a2, h_tur_b1, h_tur_b2
| by the angle (a1: the angle, or the second barrel's angle * 4 + a table
| offset): tur_tab[flips][site][angle] (tools/convert_sprites.py TURRET_SITES,
| from the original's position tables) is the pair's ref, if the PCG is
| tur_pcg's; else (another PCG base, no ref) the steps as below
h_tur_a1:
    movem.l %d4-%d6/%a1, -(%sp)
    move.w  %a1, %d4
    and.w   #0xFF, %d4
    bra.s   1f
h_tur_a2:
    movem.l %d4-%d6/%a1, -(%sp)
    move.w  %a1, %d4
    lsr.w   #2, %d4
    and.w   #0xFF, %d4
    add.w   #0x100, %d4
    bra.s   1f
h_tur_b1:
    movem.l %d4-%d6/%a1, -(%sp)
    move.w  %a1, %d4
    and.w   #0xFF, %d4
    add.w   #0x200, %d4
    bra.s   1f
h_tur_b2:
    movem.l %d4-%d6/%a1, -(%sp)
    move.w  %a1, %d4
    lsr.w   #2, %d4
    and.w   #0xFF, %d4
    add.w   #0x300, %d4
1:  move.w  ncalls, %d5
    beq     78f
    subq.w  #1, %d5
    lsl.w   #3, %d5
    lea     calls, %a1
    add.w   %d5, %a1
    move.w  %d2, %d5
    bset    #15, %d5
    cmp.w   4(%a1), %d5                 | the first barrel: the same PCG
    bne     78f
    cmp.w   6(%a1), %d3                 | and info
    bne     78f
    lea     tur_pcg, %a0
    cmp.b   0(%a0,%d4.w), %d2
    bne.s   2f
    move.w  %d3, %d5
    rol.w   #2, %d5
    and.w   #3, %d5                     | flips (bit 0 H, bit 1 V)
    lsl.w   #8, %d5
    lsl.w   #2, %d5
    add.w   %d5, %d4                    | (flips * 4 + site) * 256 + angle
    add.w   %d4, %d4
    lea     tur_tab, %a0
    move.w  0(%a0,%d4.w), %d5
    beq.s   2f
    move.w  %d5, 4(%a1)
    movem.l (%sp)+, %d4-%d6/%a1
    moveq   #1, %d0
    rts
2:  movem.l (%sp)+, %d4-%d6/%a1

h_xsp_set_tur2:
    movem.l %d4-%d6/%a1, -(%sp)
    move.w  ncalls, %d4
    beq     78f
    subq.w  #1, %d4
    lsl.w   #3, %d4
    lea     calls, %a1
    add.w   %d4, %a1
    move.w  %d2, %d4
    bset    #15, %d4
    cmp.w   4(%a1), %d4                 | the same PCG
    bne    78f
    cmp.w   6(%a1), %d3                 | the same info
    bne    78f
    move.w  (%a1), %d5
    asr.w   #6, %d5
    move.w  %d0, %d6
    asr.w   #6, %d6
    sub.w   %d5, %d6
    add.w   #16, %d6                    | dx + 16 in 0..32
    cmp.w   #32, %d6
    bhi    78f
    move.w  2(%a1), %d5
    asr.w   #6, %d5
    move.w  %d1, %d4
    asr.w   #6, %d4
    sub.w   %d5, %d4
    add.w   #16, %d4                    | dy + 16 in 0..32
    cmp.w   #32, %d4
    bhi    78f
    tst.w   %d3
    bpl.s   72f
    neg.w   %d4                         | (V flip)
    add.w   #32, %d4
72: btst    #14, %d3
    beq.s   73f
    neg.w   %d6                         | (H flip)
    add.w   #32, %d6
73: move.w  %d2, %d5
    sub.w   #TURRET_LO, %d5
    cmp.w   #TURRET_HI - TURRET_LO, %d5
    bhi    78f
    add.w   %d5, %d5
    lea     turret_rows, %a0
    move.w  0(%a0,%d5.w), %d5           | row * 33 * 33
    add.w   %d6, %d5
    add.w   %d4, %d4
    lea     mul33x2, %a0
    add.w   0(%a0,%d4.w), %d5           | + (dy + 16) * 33 + dx + 16
    moveq   #0, %d6
    move.w  %d5, %d6
    add.l   %d6, %d6
    lea     pair_lut, %a0
    move.w  0(%a0,%d6.l), %d5
    beq    78f
    add.w   #PAIR_REF - 1, %d5
    move.w  %d5, 4(%a1)
    movem.l (%sp)+, %d4-%d6/%a1
    moveq   #1, %d0
    rts
78: movem.l (%sp)+, %d4-%d6/%a1
    jmp     xsp_set_vec
turret_rows:
    .irp    r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
    .word   (TURRET_ROW + \r) * 33 * 33
    .endr
    .if     TURRET_HI - TURRET_LO - 17
    .error  "turret_rows: one per turret PCG"
    .endif
mul33x2:
    .irp    r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32
    .word   \r * 33
    .endr

| a vertical laser segment (the lasers fired straight down): 16 px above or
| below the last call of its priority, the same PCG, info and x: the two
| become one call of a synthetic ref (VLASER_REF: one 16 x 32 sprite,
| tools/convert_sprites.py), at the upper one's position
30: movem.l %d4-%d5/%a1, -(%sp)
    moveq   #63, %d4
    and.w   %d3, %d4
    lea     bhead, %a0
    moveq   #0, %d5
    move.b  0(%a0,%d4.w), %d5
    cmp.b   #0xFF, %d5
    beq.s   39f
    lsl.w   #3, %d5
    lea     calls, %a1
    add.w   %d5, %a1
    move.w  %d2, %d5
    bset    #15, %d5
    cmp.w   4(%a1), %d5                 | the same PCG (not merged yet)
    bne.s   39f
    cmp.w   6(%a1), %d3                 | the same info
    bne.s   39f
    cmp.w   (%a1), %d0                  | the same x
    bne.s   39f
    move.w  2(%a1), %d5
    sub.w   %d1, %d5
    cmp.w   #16 * 64, %d5
    beq.s   31f                         | the last one 16 px below: this one is the upper
    cmp.w   #-16 * 64, %d5
    bne.s   39f
    bra.s   32f
31: move.w  %d1, 2(%a1)
32: move.w  #VLASER_REF, 4(%a1)
    cmp.w   #VLASER_A, %d2
    beq.s   33f
    addq.w  #1, 4(%a1)                  | (the thin one)
33: movem.l (%sp)+, %d4-%d5/%a1
    moveq   #1, %d0
    rts
39: movem.l (%sp)+, %d4-%d5/%a1
    bra     29b

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
    bcc     9f
    move.w  %d3, %d4                    | (the original clobbers d0-d4)
    and.w   #63, %d4
    cmp.w   #PLAYER_LO, %d2
    bcs.s   2f
    cmp.w   #PLAYER_HI, %d2
    bcc.s   2f
    moveq   #0x3E, %d4                  | player: in front of enemies, bullets ($3C)
    bra.s   8f
2:  cmp.w   #3, %d2                     | refs 1-3 (the stage 3 boss's thick beams):
    bls     40f                         | two side by side as one (below)
    cmp.w   #EFFECT_LO, %d2
    bcs.s   8f
    cmp.w   #PARTICLE_HI, %d2
    bcc.s   8f
    cmp.w   #PARTICLE_LO, %d2
    bcc     9f                          | particles are not drawn
    cmp.w   #EFFECT_PR, %d4
    bls.s   8f
    moveq   #EFFECT_PR, %d4
8:  move.w  ncalls, %a2                | (record, inlined; d4 = priority)
    cmp.w   #MAX_CALLS, %a2
    bcc.s   9f
    move.w  %a2, %a0
    add.w   %a0, %a0
    add.w   %a0, %a0
    add.w   %a0, %a0
    add.l   #calls, %a0
    move.w  %d0, (%a0)+
    move.w  %d1, (%a0)+
    move.w  %d2, (%a0)+
    move.w  %d3, (%a0)
    move.w  %a2, %d0                    | call index
    lea     call_next, %a0              | (bhead = call_next - 128)
    move.b  -128(%a0,%d4.w), 0(%a0,%d0.w)
    move.b  %d0, -128(%a0,%d4.w)
    addq.w  #1, ncalls
    cmp.w   pr_lo, %d4
    bcc.s   1f
    move.w  %d4, pr_lo
1:  cmp.w   pr_hi, %d4
    bls.s   3f
    move.w  %d4, pr_hi
3:  move.w  %d2, %d1                    | refs src/plane.c looks for (effects, large
    lsr.w   #3, %d1                     | enemies: pl_refmap, xsp.c) -> pl_ix
    lea     pl_refmap, %a0
    btst    %d2, 0(%a0,%d1.w)
    beq.s   9f
    move.w  pl_n, %d1
    cmp.w   #PL_MAX, %d1
    bcc.s   9f
    lea     pl_ix, %a0
    move.b  %d0, 0(%a0,%d1.w)
    addq.w  #1, pl_n
9:  rts
| a beam flame (ref 1-3, 16 x 32) next to the same one 16 px to the left or
| right at the same height, among the last 4 calls of its priority: the two
| become one call of LBEAM_REF + ref - 1 (32 x 32, tools/convert_sprites.py
| LBEAM_REFS) at the left one's position
40: tst.w   %d2
    beq     8b
    move.l  %d5, -(%sp)
    lea     bhead, %a0
    moveq   #0, %d5
    move.b  0(%a0,%d4.w), %d5
    swap    %d5
    move.w  #3, %d5                     | (high word: the count)
    swap    %d5
41: cmp.b   #0xFF, %d5
    beq.s   48f
    move.w  %d5, %a1
    lsl.w   #3, %d5
    lea     calls, %a0
    add.w   %d5, %a0
    cmp.w   4(%a0), %d2
    bne.s   45f
    cmp.w   6(%a0), %d3
    bne.s   45f
    cmp.w   2(%a0), %d1                 | the same height
    bne.s   45f
    move.w  (%a0), %d5
    sub.w   %d0, %d5
    asr.w   #6, %d5
    cmp.w   #16, %d5
    beq.s   43f                         | this one on the left
    cmp.w   #-16, %d5
    beq.s   44f
45: move.w  %a1, %d5
    lea     call_next, %a0
    move.b  0(%a0,%d5.w), %d5
    and.w   #0xFF, %d5
    sub.l   #0x10000, %d5
    bpl.s   41b
48: move.l  (%sp)+, %d5
    bra     8b
43: move.w  %d0, (%a0)
44: add.w   #LBEAM_REF - 1, %d2
    move.w  %d2, 4(%a0)
    move.l  (%sp)+, %d5
    rts


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
    clr.w   dbl_n
    clr.w   dbl_rn
    move.w  #0xFFFF, pcg_lkey
1:  move.w  (%sp), %d0
    lea     bhead, %a1
41: cmp.w   pr_lo, %d0                  | (empty priorities skipped here)
    blt     90f
    cmp.b   #0xFF, 0(%a1,%d0.w)
    bne.s   42f
    subq.w  #1, %d0
    bra.s   41b
42: move.w  %d0, (%sp)
    tst.b   plane_behind                | the stage 5 boss on plane A (src/plane.c): the
    beq.s   43f                         | sprites of $33 (its arms, hexagons) low priority,
    lea     pal_attr, %a1               | behind its high priority tiles
    moveq   #15, %d1
    cmp.w   #BEHIND_PR, %d0
    bne.s   45f
44: bclr    #7, (%a1)
    addq.l  #2, %a1
    dbra    %d1, 44b
    bra.s   43f
45: bset    #7, (%a1)
    addq.l  #2, %a1
    dbra    %d1, 45b
43: move.w  #MAX_HW, hw_limit
    move.w  #0x7FFF, fx_cap
    cmp.w   #0x3F, %d0                  | the front ($3F: the bomb ...) leaves room
    bne.s   3f                          | for the player ship ($3E, 2 pieces)
    move.w  #MAX_HW - PLAYER_ROOM, hw_limit
3:  cmp.w   #BULLET_PR, %d0             | the enemy bullets (lasers) leave room for
    bne.s   3f                          | the enemies ($34, drawn after them)
    bsr     enemy_room
3:  cmp.w   #EFFECT_PR, %d0
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
    bcc     82f
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
    sub.w   #16, %d0                    | sx (-16..255: h_xsp_set keeps 0 <= x < $4400)
    move.w  (%a3)+, %d1
    asr.w   #6, %d1
    sub.w   #16, %d1                    | sy
    cmp.w   #223, %d1                   | sy <= 223 (>= -16 as x)
    bgt     70f
18: add.w   %d2, %d2                    | (bit 15 shifted out)
    lea     pcg_block, %a1
    move.w  0(%a1,%d2.w), %d4
    move.w  2(%a3), %d3                 | info
    move.w  %d3, %d2
    lsr.w   #8, %d2
    and.w   #15, %d2                    | palette
    lsl.w   #4, %d4
    or.w    %d2, %d4                    | key
    cmp.w   pcg_lkey, %d4               | (the same as the last single PCG: its
    bne.s   19f                         | attribute; bullets come in runs)
    move.w  pcg_lattr, %d4
    bra.s   13f
19: move.w  %d4, pcg_ckey
    move.w  %d4, %d5
    lsr.w   #7, %d5
    eor.w   %d4, %d5
    and.w   #511, %d5
    lsl.w   #3, %d5
    cmp.w   0(%a4,%d5.w), %d4
    bne     12f
    move.w  %a2, 4(%a4,%d5.w)
    move.w  2(%a4,%d5.w), %d4
11: add.w   %d2, %d2
    lea     pal_attr, %a1
    or.w    0(%a1,%d2.w), %d4
    move.w  %d4, pcg_lattr
    move.w  pcg_ckey, pcg_lkey
13: and.w   #0xC000, %d3
    lsr.w   #3, %d3                     | flips -> attr bits 11/12
    or.w    %d3, %d4
    cmp.w   #BULLET_PR, 2(%sp)
    beq.s   50f
51: add.w   #128, %d1
    move.w  %d1, (%a0)+
    addq.w  #1, %d7
    move.w  %d7, %d3
    or.w    #0x0500, %d3                | 2x2 cells | link
    move.w  %d3, (%a0)+
    move.w  %d4, (%a0)+
    add.w   #128, %d0
    move.w  %d0, (%a0)+
    tst.b   pair_two
    bne     17f
    bra     70f
| an enemy bullet: below line DBL_B into the doubler's list (written over the
| slots of bullets above line DBL_A by the H-int), above DBL_A: its slot noted
50: tst.b   pair_two
    bne.s   51b
    tst.b   dbl_on
    beq.s   51b
    cmp.w   #DBL_B, %d1
    blt.s   52f
    move.w  dbl_n, %d3
    cmp.w   #DBL_MAX, %d3
    bcc.s   51b
    addq.w  #1, dbl_n
    lsl.w   #3, %d3
    lea     dbl_b, %a1
    add.w   %d3, %a1
    add.w   #128, %d1
    move.w  %d1, (%a1)+
    move.w  #0x0500, (%a1)+             | (the link: the slot's, src/xsp.c)
    move.w  %d4, (%a1)+
    add.w   #128, %d0
    move.w  %d0, (%a1)
    bra     70f
52: cmp.w   #DBL_A - 16, %d1
    bgt.s   51b
    move.w  dbl_rn, %d3
    cmp.w   #DBL_MAX, %d3
    bcc.s   51b
    addq.w  #1, dbl_rn
    lea     dbl_slot, %a1
    move.b  %d7, 0(%a1,%d3.w)
    bra     51b
12: bsr     lookup_slow
    bne     11b
    sf      pair_two
    bra     70f
17: sf      pair_two                    | the second PCG of a pair not composed:
    cmp.w   hw_limit, %d7               | the same tile at the offset
    bcc     70f
    moveq   #63, %d1
    and.w   -2(%a3), %d1
    sub.w   #16, %d1
    add.w   -8(%a0), %d1
    move.w  %d1, (%a0)+
    addq.w  #1, %d7
    move.w  %d7, %d3
    or.w    #0x0500, %d3
    move.w  %d3, (%a0)+
    move.w  -8(%a0), (%a0)+
    moveq   #63, %d1
    and.w   -4(%a3), %d1
    sub.w   #16, %d1
    add.w   -8(%a0), %d1
    move.w  %d1, (%a0)+
    bra     70f

| -- a pair of PCGs: one 32x32 sprite (xsp.c pair_tile composes it)
16: movem.l %d0-%d1/%a0-%a1, -(%sp)    | (C clobbers d0 / d1 / a0 / a1)
    moveq   #0, %d0
    move.w  16(%sp), %d0                | the call index
    move.l  %d0, -(%sp)
    jsr     pair_tile
    addq.l  #4, %sp
    move.w  %d0, %d4                    | tile, 0: not now (the two as they are)
    movem.l (%sp)+, %d0-%d1/%a0-%a1
    tst.w   %d4
    bne.s   19f
    st      pair_two
    bra     18b
19:
    add.w   pair_ox, %d0
    add.w   pair_oy, %d1
    move.w  2(%a3), %d3
    lsr.w   #8, %d3
    and.w   #15, %d3
    add.w   %d3, %d3
    lea     pal_attr, %a1
    or.w    0(%a1,%d3.w), %d4           | (the flips are in the composite)
    add.w   #128, %d1
    move.w  %d1, (%a0)+
    addq.w  #1, %d7
    move.w  %d7, %d3
    or.w    #0x0F00, %d3                | 4x4 cells | link
    move.w  %d3, (%a0)+
    move.w  %d4, (%a0)+
    add.w   #128, %d0
    move.w  %d0, (%a0)+
    bra     70f

| -- text cell (x, y in pixels), or a pair (info bit 12, h_xsp_set_boss)
20: btst    #4, 6(%a3)
    bne.s   15f
    move.w  2(%a3), %d1
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
15: bclr    #14, %d2                    | the pair's first PCG: as a single one
    move.w  (%a3)+, %d0
    asr.w   #6, %d0
    sub.w   #16, %d0
    move.w  (%a3)+, %d1
    asr.w   #6, %d1
    sub.w   #16, %d1
    cmp.w   #223, %d1
    bgt     70f
    bra     16b

| -- composite ref
| -- the synthetic pairs (one piece each: angled lasers, thick beams, turret barrels;
| not the needles, enemy bullets: the general path's doubler): a single PCG's
| work, without the effect / share checks
30: cmp.w   #PAIR_REF, %d2
    bcs    64f
    cmp.w   #NREF, %d2                  | (the synthetic refs are the last ones)
    bcc    64f
    cmp.w   #BULLET_PR, 2(%sp)
    beq    64f
    add.w   %d2, %d2
    lea     ref_start, %a1
    moveq   #0, %d0
    move.w  0(%a1,%d2.w), %d0
    lsl.l   #3, %d0
    lea     ref_pieces, %a1
    add.l   %d0, %a1
    move.w  (%a1)+, %d0                 | dx
    move.w  (%a1)+, %d1                 | dy
    moveq   #0, %d2
    move.b  (%a1)+, %d2                 | size | flips << 4
    moveq   #0, %d3
    move.b  (%a1)+, %d3                 | palette xor
    move.w  (%a1), %d4                  | block
    move.w  6(%a3), %d6                 | info
    bpl.s   61f
    move.w  %d2, %d5                    | V flip: mirror around the centre
    and.w   #15, %d5
    add.w   %d5, %d5
    neg.w   %d1
    sub.w   #16, %d1
    lea     size_h, %a1
    sub.w   0(%a1,%d5.w), %d1
    eor.w   #0x20, %d2
61: btst    #14, %d6
    beq.s   62f
    move.w  %d2, %d5                    | H flip
    and.w   #15, %d5
    add.w   %d5, %d5
    neg.w   %d0
    sub.w   #16, %d0
    lea     size_w, %a1
    sub.w   0(%a1,%d5.w), %d0
    eor.w   #0x10, %d2
62: move.w  (%a3), %d5
    asr.w   #6, %d5
    add.w   %d5, %d0                    | sx
    move.w  %d0, %d5
    add.w   #32, %d5
    cmp.w   #288, %d5                   | -32 < sx < 256
    bcc     70f
    move.w  2(%a3), %d5
    asr.w   #6, %d5
    add.w   %d5, %d1                    | sy
    move.w  %d1, %d5
    add.w   #32, %d5
    cmp.w   #256, %d5                   | -32 < sy < 224
    bcc     70f
    lsr.w   #8, %d6
    eor.w   %d6, %d3
    and.w   #15, %d3                    | palette
    lsl.w   #4, %d4
    or.w    %d3, %d4                    | key
    move.w  %d4, %d5
    lsr.w   #7, %d5
    eor.w   %d4, %d5
    and.w   #511, %d5
    lsl.w   #3, %d5
    cmp.w   0(%a4,%d5.w), %d4
    beq.s   63f
    bsr     lookup_slow                 | (d4 = the tile)
    beq     70f
    bra.s   65f
63: move.w  %a2, 4(%a4,%d5.w)
    move.w  2(%a4,%d5.w), %d4
65: add.w   %d3, %d3
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
    bra     70f
| -- a ref
64: move.w  (%a3), %d0
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
    bcs.s   26f
    cmp.w   #EXPL4_HI, %d2
    bcc.s   29f
    or.w    #0x100, %d6                 | bit 8: draw every other piece (if needed)
    bra.s   29f
26: cmp.w   #ARC_LO, %d2
    bcs.s   29f
    cmp.w   #ARC_HI, %d2
    bcc.s   29f
    or.w    #0x200, %d6                 | bit 9: every other piece, phase by frame
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
    beq.s   38f
    btst    #0, 1(%sp)                  | odd pieces-left count: skipped
    beq.s   39f
37: addq.l  #8, %a3
    bra     68f
38: btst    #9, %d6
    beq.s   39f
    move.w  (%sp), %d5
    add.w   %a2, %d5                    | + xframe: the other half next frame
    btst    #0, %d5
    bne.s   37b
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
    bcc     68f
    add.w   %a6, %d1                    | sy
    move.w  %d1, %d5
    add.w   #32, %d5
    cmp.w   #256, %d5                   | -32 < sy < 224
    bcc     68f
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
    bne     38f
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
    cmp.w   #BULLET_PR, 6(%sp)          | a bullet ref's piece below DBL_B (the needle
    bne.s   66f                         | pairs): into the doubler's list, its size kept
    tst.b   dbl_on
    beq.s   66f
    cmp.w   #DBL_B, %d1
    blt.s   66f
    move.w  dbl_n, %d5
    cmp.w   #DBL_MAX, %d5
    bcc.s   66f
    addq.w  #1, dbl_n
    lsl.w   #3, %d5
    lea     dbl_b, %a1
    add.w   %d5, %a1
    add.w   #128, %d1
    move.w  %d1, (%a1)+
    move.w  %d2, (%a1)+                 | (the link: the slot's, src/xsp.c)
    move.w  %d4, (%a1)+
    add.w   #128, %d0
    move.w  %d0, (%a1)
    bra.s   68f
66: add.w   #128, %d1
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
38: bsr     lookup_slow
    bne     37b
    bra.s   68b

| -- next call in this priority
70: move.w  (%sp)+, %d6
    lea     call_next, %a1
    move.b  0(%a1,%d6.w), %d6
    bra     2b
| the table full in the enemy bullets with the doubler on: the rest still
| scanned for bullets below DBL_B (into the doubler's list, no table slot)
82: cmp.w   #BULLET_PR, (%sp)
    bne     80f
    tst.b   dbl_on
    beq     80f
83: cmp.b   #0xFF, %d6
    beq     80f
    cmp.w   #DBL_MAX, dbl_n
    bcc     80f
    move.w  %d6, %d5
    lsl.w   #3, %d5
    lea     calls, %a3
    add.w   %d5, %a3
    move.w  4(%a3), %d2
    bpl     89f                         | (a ref)
    btst    #14, %d2
    bne     89f                         | (a pair / text cell)
    move.w  2(%a3), %d1
    asr.w   #6, %d1
    sub.w   #16, %d1                    | sy
    cmp.w   #DBL_B, %d1
    blt     89f
    cmp.w   #223, %d1
    bgt     89f
    move.w  (%a3), %d0
    asr.w   #6, %d0
    sub.w   #16, %d0                    | sx
    add.w   %d2, %d2
    lea     pcg_block, %a1
    move.w  0(%a1,%d2.w), %d4
    move.w  6(%a3), %d3                 | info
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
    bne.s   87f
    move.w  %a2, 4(%a4,%d5.w)
    move.w  2(%a4,%d5.w), %d4
86: add.w   %d2, %d2
    lea     pal_attr, %a1
    or.w    0(%a1,%d2.w), %d4
    and.w   #0xC000, %d3
    lsr.w   #3, %d3
    or.w    %d3, %d4                    | attr
    move.w  dbl_n, %d3
    addq.w  #1, dbl_n
    lsl.w   #3, %d3
    lea     dbl_b, %a1
    add.w   %d3, %a1
    add.w   #128, %d1
    move.w  %d1, (%a1)+
    move.w  #0x0500, (%a1)+
    move.w  %d4, (%a1)+
    add.w   #128, %d0
    move.w  %d0, (%a1)
    bra.s   89f
87: bsr     lookup_slow
    bne.s   86b
89: lea     call_next, %a1
    move.b  0(%a1,%d6.w), %d6
    bra     83b
80: cmp.w   #BULLET_PR, (%sp)
    bne.s   81f
85: move.w  dbl_rn, %d3                 | more bullets below than slots above: empty
    cmp.w   dbl_n, %d3                  | entries for them (y 0: off the screen)
    bcc.s   81f
    cmp.w   hw_limit, %d7
    bcc.s   81f
    addq.w  #1, dbl_rn
    lea     dbl_slot, %a1
    move.b  %d7, 0(%a1,%d3.w)
    clr.w   (%a0)+
    addq.w  #1, %d7
    move.w  %d7, %d3
    or.w    #0x0500, %d3
    move.w  %d3, (%a0)+
    clr.w   (%a0)+
    move.w  #1, (%a0)+
    bra.s   85b
81: subq.w  #1, (%sp)
    bra     1b
90: addq.l  #2, %sp
    tst.b   plane_behind
    beq.s   92f
    lea     pal_attr, %a1               | (high priority again)
    moveq   #15, %d1
93: bset    #7, (%a1)
    addq.l  #2, %a1
    dbra    %d1, 93b
92: tst.w   %d7
    beq.s   91f
    clr.b   -5(%a0)                     | end of the link chain
91: add.l   %d7, stat_out
    move.w  %d7, %d0
    movem.l (%sp)+, %d2-%d7/%a2-%a6
    rts

| hw_limit for the enemy bullets: MAX_HW less the sprites of the enemies
| ($34: a ref's pieces, a PCG one), ENEMY_ROOM at most (several laser
| enemies' beams took every sprite and all of them vanished); the doubler
| still brings back bullets below its line.  (d1-d4, a1, a3, a6)
enemy_room:
    movem.l %d5/%a5, -(%sp)
    moveq   #0, %d1
    moveq   #ENEMY_ROOM, %d5
    lea     bhead + ENEMY_PR, %a5
    bsr.s   10f
    move.w  %d2, %d1
    tst.b   plane_turrets               | the stage 5 battleship on plane A: its
    beq.s   4f                          | turrets ($33) too, TURRET_ROOM at most
    moveq   #TURRET_ROOM, %d5
    lea     bhead + BEHIND_PR, %a5
    bsr.s   10f
4:  neg.w   %d1
    add.w   #MAX_HW, %d1
    move.w  %d1, hw_limit
    movem.l (%sp)+, %d5/%a5
    rts
| d1 += the sprites of list (a5), d5 at most (d2 = them)
10: move.w  %d1, -(%sp)
    lea     ref_start, %a6
    moveq   #0, %d2
    moveq   #0, %d1
    move.b  (%a5), %d1
    lea     calls, %a1
    lea     call_next, %a3
1:  cmp.b   #0xFF, %d1
    beq.s   3f
    move.w  %d1, %d3
    lsl.w   #3, %d3
    move.w  4(%a1,%d3.w), %d4
    bmi.s   2f                          | a PCG
    add.w   %d4, %d4
    add.w   2(%a6,%d4.w), %d2           | a ref: its pieces
    sub.w   0(%a6,%d4.w), %d2
    subq.w  #1, %d2
2:  addq.w  #1, %d2
    cmp.w   %d5, %d2
    bcc.s   3f
    move.b  0(%a3,%d1.w), %d1           | next
    bra.s   1b
3:  cmp.w   %d5, %d2
    bls.s   4f
    move.w  %d5, %d2
4:  move.w  (%sp)+, %d1
    add.w   %d2, %d1
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

| pair_put(dst, src, lut, ox, oy, flips) (src/xsp.c pair_tile): a 16x16 PCG
| into the 32x32 composite at dst (MD sprite tile order: 8x8 tiles column by
| column) at (ox, oy) (0..16), flipped (flips bit 0: onto an empty one).  The
| rows first converted (lut: a byte of two pixels) and flipped into a buffer,
| shifted by a pixel for an odd ox, then laid over the composite a byte at a
| time where not 0
    .globl  pair_put
    .set    PP_ARGS, 44 + 180 + 4
pair_put:
    movem.l %d2-%d7/%a2-%a6, -(%sp)
    lea     -180(%sp), %sp              | 16 rows x 10 bytes, the 9 byte offsets
    move.l  PP_ARGS(%sp), %a0           | dst
    move.l  PP_ARGS + 4(%sp), %a2       | src
    move.l  PP_ARGS + 8(%sp), %a3       | lut
    move.w  PP_ARGS + 14(%sp), %d4      | ox
    move.w  PP_ARGS + 18(%sp), %d5      | oy
    move.w  PP_ARGS + 22(%sp), %d6      | flips
    move.l  %sp, %a1
    moveq   #0, %d7                     | row
1:  move.w  %d7, %d0
    tst.w   %d6
    bpl.s   2f
    eor.w   #15, %d0                    | (V flip)
2:  move.w  %d0, %d1
    and.w   #7, %d1
    lsl.w   #2, %d1
    lsr.w   #3, %d0
    lsl.w   #5, %d0
    add.w   %d1, %d0
    lea     0(%a2,%d0.w), %a4           | the row in the left tiles (the right: + 64)
    moveq   #0, %d1
    btst    #14, %d6
    bne.s   3f
    .rept   4
    move.b  (%a4)+, %d1
    move.b  0(%a3,%d1.w), (%a1)+
    .endr
    lea     60(%a4), %a4
    .rept   4
    move.b  (%a4)+, %d1
    move.b  0(%a3,%d1.w), (%a1)+
    .endr
    bra.s   4f
3:  addq.l  #8, %a1                     | H flip: backwards, the pixels of a byte swapped
    .rept   4
    move.b  (%a4)+, %d1
    move.b  0(%a3,%d1.w), %d0
    rol.b   #4, %d0
    move.b  %d0, -(%a1)
    .endr
    lea     60(%a4), %a4
    .rept   4
    move.b  (%a4)+, %d1
    move.b  0(%a3,%d1.w), %d0
    rol.b   #4, %d0
    move.b  %d0, -(%a1)
    .endr
    addq.l  #8, %a1
4:  btst    #0, %d4
    beq.s   5f
    move.l  -8(%a1), %d0                | odd ox: a pixel to the right (9 bytes)
    move.l  -4(%a1), %d1
    move.b  %d1, %d2
    lsl.b   #4, %d2
    move.b  %d2, (%a1)
    lsr.l   #4, %d1
    moveq   #15, %d2
    and.l   %d0, %d2
    ror.l   #4, %d2
    or.l    %d2, %d1
    lsr.l   #4, %d0
    move.l  %d0, -8(%a1)
    move.l  %d1, -4(%a1)
5:  addq.l  #2, %a1
    addq.w  #1, %d7
    cmp.w   #16, %d7
    bne     1b
    lea     160(%sp), %a5               | composite offsets of the bytes of a row
    move.w  %d4, %d0
    lsr.w   #1, %d0                     | byte k = ox / 2 + j: (k / 4) * 128 + k % 4
    moveq   #7, %d3
    btst    #0, %d4
    beq.s   6f
    moveq   #8, %d3
6:  move.w  %d3, %d7
7:  move.w  %d0, %d1
    and.w   #3, %d1
    move.w  %d0, %d2
    lsr.w   #2, %d2
    lsl.w   #7, %d2
    add.w   %d2, %d1
    move.w  %d1, (%a5)+
    addq.w  #1, %d0
    dbra    %d7, 7b
    lea     nib_mask, %a6
    move.l  %sp, %a1
    moveq   #15, %d7
8:  move.w  %d5, %d0                    | composite row oy + row
    move.w  %d0, %d1
    and.w   #7, %d1
    lsl.w   #2, %d1
    lsr.w   #3, %d0
    lsl.w   #5, %d0
    add.w   %d1, %d0
    lea     0(%a0,%d0.w), %a4
    lea     160(%sp), %a5
    move.l  %a1, %a2
    move.w  %d3, %d6
    moveq   #0, %d0
    btst    #0, PP_ARGS + 23(%sp)
    beq.s   9f
11: move.w  (%a5)+, %d1                 | (onto an empty composite)
    move.b  (%a2)+, 0(%a4,%d1.w)
    dbra    %d6, 11b
    bra.s   12f
9:  move.w  (%a5)+, %d1
    move.b  (%a2)+, %d0
    beq.s   10f
    move.b  0(%a6,%d0.w), %d2
    not.b   %d2
    and.b   %d2, 0(%a4,%d1.w)
    or.b    %d0, 0(%a4,%d1.w)
10: dbra    %d6, 9b
12: lea     10(%a1), %a1
    addq.w  #1, %d5
    dbra    %d7, 8b
    lea     180(%sp), %sp
    movem.l (%sp)+, %d2-%d7/%a2-%a6
    rts

| Level 4 H-int (src/plane.c plane_vblank: the first, at the latest at the
| first line with something to do; then each H-int sets register 10 for the
| one after the next: at the first line with something to do after that, 30:
| below; in the H-blank before it).  The line from the V counter (+ 1: a
| late H-int, held off while the VBlank handler or the sound masks it, does
| its work late rather than not at all; it plans from the line it sees, so
| the H-ints come no later than planned):
| * split_line (0: none): plane A's scroll from that line on is split_vs /
|   split_hs: the second image (the second of two battleships)
| * act_fm_ev from fm_evi (formation mode, src/plane.c fm_ev; a line 0xFF
|   after the last): plane A's scroll for the next band of units
| * DBL_A: the sprite doubler.  The bullets above line DBL_A have been
|   shown: their slots in the sprite table take the bullets below DBL_B
|   (xsp_upload sets act_*; hint_n is the count still to write this frame).
|   VRAM writes in the active display: 2 entries a line.
| Nothing left: the H-int off (hint_nx 0xFF).
    .globl  _hint
_hint:
    move.l  %d0, -(%sp)
    moveq   #0, %d0
    move.b  0xC00008, %d0               | V counter
    cmp.b   hint_nx, %d0                | (nothing due before hint_nx)
    bcs     20f
    cmp.b   #0xE0, %d0                  | (in the VBlank: a stale H-int, H-int
    bcc     8f                          | just turned on)
    addq.w  #1, %d0                     | the line about to be drawn
    movem.l %d1-%d4/%a0, -(%sp)
    tst.w   split_line
    beq.s   3f
    cmp.w   split_line, %d0
    bcs.s   3f
    clr.w   split_line
    move.l  #0x40000010, 0xC00004       | VSRAM 0: plane A
    move.w  split_vs, 0xC00000
    move.l  #0x40000000 + ((VRAM_HSCROLL & 0x3FFF) << 16) + (VRAM_HSCROLL >> 14), 0xC00004
    move.w  split_hs, 0xC00000
3:  moveq   #0, %d1
    move.b  fm_evi, %d1
    lea     act_fm_ev, %a0
    move.w  %d1, %d2
    lsl.w   #2, %d2
    add.w   %d2, %a0
    cmp.b   (%a0), %d0                  | (formation mode: the first one's line not
    bcs.s   6f                          | reached)
5:  move.l  #0x40000010, 0xC00004       | (the next band: plane A's vertical scroll,
    move.w  2(%a0), 0xC00000            | its horizontal scroll)
    move.l  #0x40000000 + ((VRAM_HSCROLL & 0x3FFF) << 16) + (VRAM_HSCROLL >> 14), 0xC00004
    moveq   #0, %d2
    move.b  1(%a0), %d2
    move.w  %d2, 0xC00000
    addq.l  #4, %a0
    addq.b  #1, %d1
    cmp.b   (%a0), %d0
    bcc.s   5b
    move.b  %d1, fm_evi
6:  tst.w   hint_n
    beq.s   30f
    cmp.w   #DBL_A, %d0
    bcs.s   30f
    movem.l %a1-%a2, -(%sp)
    move.w  hint_n, %d3
    clr.w   hint_n
    subq.w  #1, %d3
    lea     act_slot, %a1
    lea     act_b, %a2
    lea     0xC00004, %a0
2:  moveq   #0, %d1
    move.b  (%a1)+, %d1
    lsl.w   #3, %d1
    add.w   #0xD800 & 0x3FFF, %d1       | (VRAM_SPRITES)
    or.w    #0x4000, %d1
    move.w  %d1, (%a0)
    move.w  #0xD800 >> 14, (%a0)
    move.l  (%a2)+, -4(%a0)
    move.l  (%a2)+, -4(%a0)
    dbra    %d3, 2b
    movem.l (%sp)+, %a1-%a2
| from line d0: the next line due (d1: hint_nx), the first line with
| something to do after the next H-int (d2: register 10 for the reload there,
| at the most 248 lines on)
30: move.w  #0x100, %d1
    move.w  #0x100, %d2
    move.w  hint_exp, %d3               | the line this H-int came at: as planned (one
    cmp.w   %d0, %d3                    | held off, late: the reload was at that line;
    bls.s   31f                         | several: the last of them), else this one
    move.w  %d0, %d3
31: move.w  %d3, %d4
    add.w   hint_r1, %d4
    cmp.w   %d0, %d4
    bhi.s   32f
    move.w  %d4, %d3
    bra.s   31b
32: add.w   hint_r1, %d3                | (the next H-int's line)
    move.w  %d3, hint_exp
    move.w  split_line, %d4
    beq.s   1f
    move.w  %d4, %d1
    cmp.w   %d3, %d4
    bls.s   1f
    move.w  %d4, %d2
1:  tst.w   hint_n
    beq.s   1f
    move.w  #DBL_A, %d4
    cmp.w   %d4, %d1
    bls.s   2f
    move.w  %d4, %d1
2:  cmp.w   %d3, %d4
    bls.s   1f
    cmp.w   %d4, %d2
    bls.s   1f
    move.w  %d4, %d2
1:  moveq   #0, %d4
    move.b  fm_evi, %d4
    lea     act_fm_ev, %a0
    lsl.w   #2, %d4
    add.w   %d4, %a0
    moveq   #0, %d4
    move.b  (%a0), %d4
    cmp.w   %d4, %d1
    bls.s   3f
    move.w  %d4, %d1
3:  cmp.w   %d3, %d4
    bhi.s   4f
    cmp.b   #0xFF, %d4
    beq.s   5f
    addq.l  #4, %a0
    move.b  (%a0), %d4
    bra.s   3b
4:  cmp.w   %d4, %d2
    bls.s   5f
    move.w  %d4, %d2
5:  cmp.w   #0xE0, %d1
    bcs.s   1f
    move.b  #0xFF, hint_nx              | nothing more this frame: the H-int off
    move.w  #0x8004, 0xC00004           | (src/plane.c plane_vblank turns it on)
    bra.s   9f
1:  subq.w  #1, %d1                     | (the V counter: the line before)
    bcc.s   1f
    moveq   #0, %d1
1:  move.b  %d1, hint_nx
    sub.w   %d3, %d2
    cmp.w   #0xF8, %d2
    bls.s   1f
    move.w  #0xF8, %d2
1:  cmp.w   hint_r1, %d2
    beq.s   9f
    move.w  %d2, hint_r1
    subq.w  #1, %d2
    or.w    #0x8A00, %d2
    move.w  %d2, 0xC00004
9:  movem.l (%sp)+, %d1-%d4/%a0
8:  move.l  (%sp)+, %d0
    rte
20: addq.w  #1, %d0                     | (nothing due: only the planning)
    movem.l %d1-%d4/%a0, -(%sp)
    bra     30b

| xsp_route(on) (src/xsp.c xsp_pairs): laser pairs (h_xsp_set_boss) or not
    .globl  xsp_route
xsp_route:
    move.l  #h_xsp_set_main, %d0
    tst.b   7(%sp)
    beq.s   1f
    move.l  #h_xsp_set_boss, %d0
1:  move.l  %d0, xsp_set_vec + 2
    rts

    .data
    .balign 2
xsp_set_vec: .word 0x4EF9               | jmp h_xsp_set_main / h_xsp_set_boss
    .long   h_xsp_set_main
    .set    h_xsp_set, xsp_set_vec

    .section .note.GNU-stack,"",%progbits

    .bss
    .globl  stat_pieces, stat_calls, stat_out, stat_rec, pl_ix, pl_n, bhead, call_next
    .balign 2
bhead:       .space 128                 | list head per priority (0xFF: empty), then
call_next:   .space 256                 | the next call: one base register reaches both
rec_pr:      .word 0
pl_n:        .word 0                    | calls src/plane.c looks at: count, indices
pl_ix:       .space 64
hw_limit:    .word 0
fx_cap:      .word 0
pair_two:    .byte 0
    .balign 2
pcg_lkey:    .word 0                    | xsp_build: the last single PCG's key, attribute
pcg_lattr:   .word 0
pcg_ckey:    .word 0                    | xsp_build: the pair at hand drawn as two sprites
    .balign 2
    .globl  dbl_n, dbl_rn, dbl_slot, dbl_b, dbl_on
dbl_on:      .byte 0                    | (src/xsp.c: sprites have been running short)
    .balign 2
dbl_n:       .word 0                    | the doubler (xsp_build): bullets below DBL_B,
dbl_rn:      .word 0                    | slots of bullets above DBL_A (and empty ones)
dbl_b:       .space DBL_MAX * 8
dbl_slot:    .space DBL_MAX
    .balign 2
    .globl  act_n, hint_n, act_slot, act_b, act_a, split_line, split_vs, split_hs
needle_try:  .byte 0                    | h_xsp_set_main: a needle pair being looked for
    .balign 2
    .globl  hint_nx, hint_r1, hint_exp
hint_nx:     .byte 0                    | _hint: the first line to look at (V counter)
    .balign 2
hint_r1:     .word 0                    | (register 10 + 1: the lines from one H-int to the next)
hint_exp:    .word 0                    | (the line the next H-int comes at)
split_line:  .word 0                    | plane A split for the frame shown (_hint):
split_vs:    .word 0                    | from this line on (0: none) this scroll
split_hs:    .word 0
act_n:       .word 0                    | the doubler for the frame shown (xsp_upload):
hint_n:      .word 0                    | entries, still to write (_hint),
act_b:       .space DBL_MAX * 8         | the bullets below, the ones above (VBlank
act_a:       .space DBL_MAX * 8         | puts them back when no new table comes),
act_slot:    .space DBL_MAX             | slots
    .balign 2
stat_pieces: .long 0
stat_calls:  .long 0
stat_out:    .long 0
stat_rec:    .long 0
