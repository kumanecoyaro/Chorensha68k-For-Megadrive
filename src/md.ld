OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)

/* RAM $FF0000-$FF9BFF holds the original game's variables (tools/relocate.py,
 * ORIG_RAM / ORIG_RAM_SIZE, checked in src/main.c); the port's own data
 * starts at $FF9C00, the stack is at the top. */
MEMORY
{
    rom (rx)  : ORIGIN = 0x000000, LENGTH = 4M
    ram (rwx) : ORIGIN = 0xFF9C00, LENGTH = 0x6200
}

SECTIONS
{
    /* vectors $000, header $100, hook table $200 (HOOK_BASE) */
    .boot 0 : {
        KEEP(*(.vectors))
        KEEP(*(.header))
        KEEP(*(.hooks))
    } > rom

    /* the relocated SZ2.X image (ORIG_ROM) */
    .orig 0x10000 : {
        KEEP(*(.orig))
    } > rom

    .text : {
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

    ASSERT(SIZEOF(.boot) <= 0x10000, "boot section overlaps ORIG_ROM")

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
