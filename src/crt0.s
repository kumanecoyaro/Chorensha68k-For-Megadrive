| Mega Drive startup: vector table, ROM header, hardware init, C runtime init.

    .section .vectors, "ax"
    .globl  _start
    .long   0x00FFFE00          | initial SSP
    .long   _start              | reset PC
    .long   _buserr             | bus error
    .long   _addrerr            | address error
    .long   _illegal            | illegal instruction
    .rept   19
    .long   _exception
    .endr
    .long   _exception          | spurious
    .long   _exception          | level 1
    .long   _exception          | level 2 (ext)
    .long   _exception          | level 3
    .long   _hint               | level 4 (HBlank)
    .long   _exception          | level 5
    .long   _vint               | level 6 (VBlank)
    .long   _exception          | level 7
    .rept   32
    .long   _exception          | traps / reserved
    .endr

    .section .header, "a"
    .ascii  "SEGA MEGA DRIVE "                                  | 0x100 system
    .ascii  "(C)FMBE 2026.OCT"                                  | 0x110 copyright
    .ascii  "CHO REN SHA 68K PORT TEST                       " | 0x120 domestic name (48)
    .ascii  "CHO REN SHA 68K PORT TEST                       " | 0x150 overseas name (48)
    .ascii  "GM 00000000-00"                                    | 0x180 serial
    .word   0x0000                                              | 0x18E checksum (patched)
    .ascii  "J               "                                  | 0x190 I/O support
    .long   0x00000000                                          | 0x1A0 ROM start
    .long   _rom_end_m1                                         | 0x1A4 ROM end (patched)
    .long   0x00FF0000                                          | 0x1A8 RAM start
    .long   0x00FFFFFF                                          | 0x1AC RAM end
    .ascii  "            "                                      | 0x1B0 SRAM (none)
    .ascii  "            "                                      | 0x1BC modem
    .ascii  "                                        "          | 0x1C8 notes
    .ascii  "JUE             "                                  | 0x1F0 region

    .text
_start:
    move.w  #0x2700, %sr
    | TMSS
    move.b  0xA10001, %d0
    andi.b  #0x0F, %d0
    beq.s   1f
    move.l  #0x53454741, 0xA14000           | 'SEGA'
1:
    tst.w   0xC00004                        | clear pending VDP state
    | Z80: request bus and hold reset
    move.w  #0x100, 0xA11100
    move.w  #0x100, 0xA11200
    | PSG silence
    lea     0xC00011, %a0
    move.b  #0x9F, (%a0)
    move.b  #0xBF, (%a0)
    move.b  #0xDF, (%a0)
    move.b  #0xFF, (%a0)
    | clear RAM
    lea     0xFF0000, %a0
    move.w  #0x3FFF, %d0
    moveq   #0, %d1
2:  move.l  %d1, (%a0)+
    dbra    %d0, 2b
    | copy .data
    lea     _etext, %a0
    lea     _sdata, %a1
    move.l  #_edata, %d0
    sub.l   #_sdata, %d0
    lsr.l   #1, %d0
    beq.s   4f
    subq.l  #1, %d0
3:  move.w  (%a0)+, (%a1)+
    dbra    %d0, 3b
4:
    | original game RAM (data variables / routines moved to RAM by tools/relocate.py)
    lea     orig_ram_init, %a0
    lea     0xFF0000, %a1
    move.l  #orig_ram_init_end, %d0
    sub.l   #orig_ram_init, %d0
    lsr.l   #1, %d0
    beq.s   6f
    subq.l  #1, %d0
7:  move.w  (%a0)+, (%a1)+
    dbra    %d0, 7b
6:
    lea     0x00FFFE00, %sp
    jsr     main
5:  bra.s   5b

_exception:
    rte

| fatal CPU exceptions: record the cause at $FFFFF0 and stop
_buserr:
    move.w  #2, 0xFFFFF0
    bra.s   _fatal_g0
_addrerr:
    move.w  #3, 0xFFFFF0
_fatal_g0:
    move.l  10(%sp), 0xFFFFF2               | faulting PC (group 0 frame)
    move.l  2(%sp), 0xFFFFF6                | access address
    bra.s   _halt
_illegal:
    move.w  #4, 0xFFFFF0
    move.l  2(%sp), 0xFFFFF2
_halt:
    move.w  #0x2700, %sr
1:  bra.s   1b

_vint:
    movem.l %d0-%d1/%a0-%a1, -(%sp)
    addq.w  #1, vblank_count
    addq.w  #1, ORIG_VBLCNT                 | B_4a0cc: the original's vsync counter
    move.w  0xC00004, %d0                   | ack
    jsr     vblank_work                     | src/main.c (gcc saves d2-d7/a2-a6 itself)
    movem.l (%sp)+, %d0-%d1/%a0-%a1
    rte

| 32x32 -> 32 bit multiply for gcc (no libgcc for plain 68000 is linked)
    .globl  __mulsi3
__mulsi3:
    move.w  4(%sp), %d0                     | a.hi
    mulu.w  10(%sp), %d0                    | a.hi * b.lo
    move.w  8(%sp), %d1                     | b.hi
    mulu.w  6(%sp), %d1                     | b.hi * a.lo
    add.w   %d1, %d0
    swap    %d0
    clr.w   %d0
    move.w  6(%sp), %d1
    mulu.w  10(%sp), %d1                    | a.lo * b.lo
    add.l   %d1, %d0
    rts

    .globl  _rom_end_m1
    .set    _rom_end_m1, 0x000FFFFF
    .section .note.GNU-stack,"",%progbits
