| Converted sound effect samples (8-bit unsigned, 15.625 kHz), see tools/convert_pcm.py
    .section .rodata.pcm, "a"
    .balign 2
    .globl  pcm_data
pcm_data:
    .incbin "pcm.bin"
    .section .note.GNU-stack,"",%progbits
