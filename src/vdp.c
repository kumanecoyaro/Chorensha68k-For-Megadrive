#include "vdp.h"

volatile u16 vblank_count;

static const u8 vdp_regs[24] = {
    0x04,                   /* 0: HInt off */
    0x34,                   /* 1: display off, VInt on, DMA on, mode 5, V28 */
    VRAM_PLANE_A >> 10,     /* 2: plane A */
    VRAM_WINDOW >> 10,      /* 3: window */
    VRAM_PLANE_B >> 13,     /* 4: plane B */
    VRAM_SPRITES >> 9,      /* 5: sprite table */
    0x00,
    0x00,                   /* 7: backdrop = pal 0 color 0 */
    0x00, 0x00,
    0xFF,                   /* 10: HInt counter */
    0x00,                   /* 11: full-screen H/V scroll */
    0x00,                   /* 12: H32, no interlace */
    VRAM_HSCROLL >> 10,     /* 13: hscroll table */
    0x00,
    0x02,                   /* 15: autoincrement 2 */
    0x10,                   /* 16: plane size 32x64 cells */
    0x00,                   /* 17: window H = none */
    0x01,                   /* 18: window V = top 1 row (score line) */
    0, 0, 0, 0, 0
};

void vdp_set_reg(u8 reg, u8 val)
{
    VDP_CTRL = 0x8000 | (reg << 8) | val;
}

void vdp_init(void)
{
    int i;
    (void)VDP_CTRL;
    for (i = 0; i < 19; i++)
        vdp_set_reg(i, vdp_regs[i]);
    /* clear VRAM, CRAM, VSRAM */
    vdp_fill(0, 0, 0x8000);
    VDP_CTRL32 = CRAM_W(0);
    for (i = 0; i < 64; i++)
        VDP_DATA = 0;
    VDP_CTRL32 = VSRAM_W(0);
    for (i = 0; i < 40; i++)
        VDP_DATA = 0;
}

void vdp_display(int on)
{
    vdp_set_reg(1, on ? 0x74 : 0x34);
}

void vdp_load_words(u16 vram, const u16 *src, u16 nwords)
{
    VDP_CTRL32 = VRAM_W(vram);
    while (nwords--)
        VDP_DATA = *src++;
}

void vdp_load_tiles(u16 first_tile, const u32 *src, u16 ntiles)
{
    u32 n = (u32)ntiles * 8;
    VDP_CTRL32 = VRAM_W(first_tile * 32);
    while (n--)
        VDP_DATA32 = *src++;
}

void vdp_fill(u16 vram, u16 val, u16 nwords)
{
    VDP_CTRL32 = VRAM_W(vram);
    while (nwords--)
        VDP_DATA = val;
}

void vdp_set_palettes(const u16 *pal, u16 n)
{
    VDP_CTRL32 = CRAM_W(0);
    while (n--)
        VDP_DATA = *pal++;
}

void vdp_vscroll(u16 a, u16 b)
{
    VDP_CTRL32 = VSRAM_W(0);
    VDP_DATA = a;
    VDP_DATA = b;
}

void vdp_wait_vblank(void)
{
    u16 c = vblank_count;
    while (vblank_count == c)
        ;
}

/* 3-button pad on port 1: returns PAD_* bits (1 = pressed) */
u16 pad_read(void)
{
    volatile u8 *data = (volatile u8 *)0xA10003;
    volatile u8 *ctrl = (volatile u8 *)0xA10009;
    u8 hi, lo;
    *ctrl = 0x40;
    *data = 0x40;
    __asm__ volatile("nop; nop");
    hi = *data;                         /* ?1CBRLDU */
    *data = 0x00;
    __asm__ volatile("nop; nop");
    lo = *data;                         /* ?0SA00DU */
    *data = 0x40;
    return (u16)(~((hi & 0x3F) | ((lo & 0x30) << 2)) & 0xFF);
}

/* 68000 -> VRAM DMA (src in RAM, even, within one 128 KB block) */
void vdp_dma_vram(u16 vram, const void *src, u16 words)
{
    u32 a = (u32)src >> 1;
    volatile u16 lo;
    vdp_set_reg(19, words & 0xFF);
    vdp_set_reg(20, words >> 8);
    vdp_set_reg(21, a & 0xFF);
    vdp_set_reg(22, (a >> 8) & 0xFF);
    vdp_set_reg(23, (a >> 16) & 0x7F);
    /* the command's second word is written from RAM (DMA trigger quirk) */
    lo = (u16)(((vram >> 14) & 3) | 0x80);
    VDP_CTRL = 0x4000 | (vram & 0x3FFF);
    VDP_CTRL = lo;
}
