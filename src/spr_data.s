| sprite tile blocks (tools/convert_sprites.py)
    .section .rodata
    .globl  spr_tiles
    .balign 2
spr_tiles:
    .incbin "spr_tiles.bin"
    .balign 2
    .section .note.GNU-stack,"",%progbits
