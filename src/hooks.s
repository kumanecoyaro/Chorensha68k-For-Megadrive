| Bridge between the original SZ2.X code (relocated, see tools/relocate.py)
| and the Mega Drive replacements of the X68000 libraries.
|
| .hooks sits at a fixed ROM address (HOOK_BASE, $200): the relocator patches
| each hooked library function with `jmp HOOK_BASE + 6*i`.

    .section .hooks, "ax"
    .include "hooks.inc"

    .text
| (register-argument XSP calls h_xsp_set / h_xsp_set_obj / h_xobj_set: src/xsp_fast.s)

| ---- allocation for effect objects (tools/relocate.py FX_INITS): every
| other request gets "pool empty" (d0 = a0 = 0), the rest fn_040b6;
| particles (PARTICLE_INITS) always get "pool empty"
    .globl  h_alloc_fx, h_alloc_none, fx_alloc_toggle
h_alloc_fx:
    eori.w  #1, fx_alloc_toggle
    beq.s   h_alloc_none
    jmp     0x10000 + 0x40b6                | ORIG_ROM + fn_040b6
h_alloc_none:
    moveq   #0, %d0
    suba.l  %a0, %a0
    rts

| ---- explosion sequence (class $10f, a3 = object, d0 -> class table entry):
| spawn only while the remaining count ($50, decremented after each spawn) is
| odd, so a sequence makes half as many explosions; those always get their
| object (no second halving by h_alloc_fx)
    .globl  h_seq_spawn
h_seq_spawn:
    btst    #0, 0x51(%a3)
    beq.s   1f
    movea.l %d0, %a1
    movea.l (%a1), %a0
    clr.w   fx_alloc_toggle
    jmp     (%a0)
1:  rts

    .bss
fx_alloc_toggle: .word 0
    .text

| ---- C -> original code.  The original (XC compiled) code may clobber
| d0-d2/a0-a2; gcc expects d2-d7/a2-a6 preserved.
| long ocall(void *fn, long a1, long a2, long a3, long a4)
    .globl  ocall
ocall:
    movem.l %d2-%d7/%a2-%a6, -(%sp)
    lea     48(%sp), %a1
    move.l  (%a1)+, %a0
    move.l  12(%a1), -(%sp)
    move.l  8(%a1), -(%sp)
    move.l  4(%a1), -(%sp)
    move.l  (%a1), -(%sp)
    jsr     (%a0)
    lea     16(%sp), %sp
    movem.l (%sp)+, %d2-%d7/%a2-%a6
    rts

| ---- original data / RAM image
    .section .orig, "a"
    .globl  orig_image
orig_image:
    .incbin "orig.bin"

    .section .rodata
    .globl  orig_ram_init, orig_ram_init_end
    .balign 2
orig_ram_init:
    .incbin "orig_ram.bin"
orig_ram_init_end:
    .balign 2
    .section .note.GNU-stack,"",%progbits
