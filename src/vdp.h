#ifndef VDP_H
#define VDP_H
#include "types.h"

#define VDP_DATA   (*(volatile u16 *)0xC00000)
#define VDP_DATA32 (*(volatile u32 *)0xC00000)
#define VDP_CTRL   (*(volatile u16 *)0xC00004)
#define VDP_CTRL32 (*(volatile u32 *)0xC00004)

/* VRAM layout (H40, planes 32x64 cells).  The window table is 64 cells wide
 * in H40 but only its cells 32..39 of rows 0..27 show (the score panel on the
 * right, +$40..$4F of each 128-byte row): the hscroll table (its first 4
 * bytes, full-screen scroll) and the panel's opaque glyphs (src/main.c) sit
 * in the rest.  The sprite table (80 entries) takes the last 32 tiles. */
#define VRAM_SPRITES   0xBC00
#define VRAM_PLANE_A   0xC000
#define VRAM_WINDOW    0xD000
#define VRAM_HSCROLL   0xD400
#define VRAM_PLANE_B   0xE000

#define VRAM_W(a)  (0x40000000UL | (((u32)(a) & 0x3FFF) << 16) | ((u32)(a) >> 14))
#define CRAM_W(a)  (0xC0000000UL | (((u32)(a) & 0x3FFF) << 16))
#define VSRAM_W(a) (0x40000010UL | (((u32)(a) & 0x3FFF) << 16))

#define PAD_UP    0x01
#define PAD_DOWN  0x02
#define PAD_LEFT  0x04
#define PAD_RIGHT 0x08
#define PAD_B     0x10
#define PAD_C     0x20
#define PAD_A     0x40
#define PAD_START 0x80

extern volatile u16 vblank_count;

void vdp_init(void);
void vdp_display(int on);
void vdp_set_reg(u8 reg, u8 val);
void vdp_load_words(u16 vram, const u16 *src, u16 nwords);
void vdp_load_tiles(u16 first_tile, const u32 *src, u16 ntiles);
void vdp_fill(u16 vram, u16 val, u16 nwords);
void vdp_set_palettes(const u16 *pal, u16 n);
void vdp_vscroll(u16 a, u16 b);
void vdp_wait_vblank(void);
u16 pad_read(void);
void vdp_dma_vram(u16 vram, const void *src, u16 words);

#endif
