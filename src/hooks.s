| Bridge between the original SZ2.X code (relocated, see tools/relocate.py)
| and the Mega Drive replacements of the X68000 libraries.
|
| .hooks sits at a fixed ROM address (HOOK_BASE, $200): the relocator patches
| each hooked library function with `jmp HOOK_BASE + 6*i`.

    .section .hooks, "ax"
    .include "hooks.inc"

    .text
| (register-argument XSP calls h_xsp_set / h_xsp_set_obj / h_xobj_set: src/xsp_fast.s)

| ---- particle allocation (tools/relocate.py PARTICLE_INITS): always "pool
| empty" (d0 = a0 = 0)
    .globl  h_alloc_none
h_alloc_none:
    moveq   #0, %d0
    suba.l  %a0, %a0
    rts

| ---- fn_05574 (BG full redraw, hooked by x_bg_full): its first two
| instructions (the 6 bytes replaced by the hook jump), then the rest
    .globl  orig_05574
orig_05574:
    subq.l  #8, %sp
    movem.l %d3-%d7/%a3-%a6, -(%sp)
    jmp     0x10000 + 0x557a

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
