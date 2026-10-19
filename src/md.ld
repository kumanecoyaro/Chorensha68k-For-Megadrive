OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)

MEMORY
{
    rom (rx)  : ORIGIN = 0x000000, LENGTH = 4M
    ram (rwx) : ORIGIN = 0xFF0000, LENGTH = 64K
}

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        KEEP(*(.header))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(2);
        _etext = .;
    } > rom

    .data : AT(_etext) {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(2);
        _edata = .;
    } > ram

    .bss (NOLOAD) : {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(2);
        _ebss = .;
    } > ram

    _rom_end = _etext + (_edata - _sdata);

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
