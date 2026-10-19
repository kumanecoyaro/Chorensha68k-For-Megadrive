/*
 * XSP sprite library replacement.
 *
 * The original draws with xsp_set (one 16x16 PCG) and xobj_set (a composite
 * "ref" of many PCGs) every frame; XSP sorts by priority and multiplexes up
 * to 512 sprites.  Here every call becomes Mega Drive hardware sprites built
 * from pre-converted blocks (tools/convert_sprites.py):
 *
 *   * a block's tiles are copied into a VRAM cache on first use (two slot
 *     sizes: 4 tiles and 16 tiles, least-recently-used replacement), with the
 *     X68000 palette remapped to Mega Drive palette 2 or 3 on the way;
 *   * the frame's sprites are sorted by XSP priority (info bits 0-5, larger
 *     = front) and the first 64 (H32 limit) go into the sprite table.
 *
 * Text rows other than the score line are drawn here as 8x8 sprites.
 */
#include "game.h"
#include "vdp.h"
#include "spr.h"

#define CACHE_A_BASE  404           /* VRAM tile of the first 4-tile slot */
#define CACHE_A_SLOTS 160
#define CACHE_B_BASE  (CACHE_A_BASE + CACHE_A_SLOTS * 4)
#define CACHE_B_SLOTS 30
#define NSLOT (CACHE_A_SLOTS + CACHE_B_SLOTS)
#define HASH_SIZE 512                   /* open addressing, key = block << 4 | palette */
#define KEY_EMPTY 0xFFFF
#define NO_ENTRY  0xFFFF

#define STAGE_BYTES 4096            /* tiles converted per frame, DMAed in VBlank */
#define MAX_UP 48
#define MAX_HW 64

typedef struct { u16 key; u16 tile; u16 stamp; u16 slot; } HEnt;
typedef struct { u16 vram; u16 off; u16 words; } Upload;
typedef struct { s16 y; u16 size; u16 attr; s16 x; } HwSpr;

/* shared with src/xsp_fast.s (the per-sprite fast paths) */
HEnt htab[HASH_SIZE];
u16 xframe;
typedef struct { s16 x, y; u16 id, info; } Call;
Call calls[255];                        /* the frame's xsp_set / xobj_set calls */
u8 call_next[255];                      /* per-priority lists */
u8 bhead[66];
u16 pr_lo = 64, pr_hi;
u16 ncalls;
u16 pal_attr[16];                       /* $8000 | MD palette << 13 for each X68000 palette */

static u16 slot_ent[NSLOT];             /* hash entry holding the slot, NO_ENTRY = free */
static u16 clock_a, clock_b;
u16 xsp_misses;

static u8 stage[STAGE_BYTES];
static u16 stage_len;
static Upload up[MAX_UP];
static u8 nup;

static HwSpr table[MAX_HW];
static u8 ntable, table_ready;

/* ------------------------------------------------------------ VRAM cache */
static inline u16 hslot(u16 key)
{
    return (key ^ (key >> 7)) & (HASH_SIZE - 1);
}

static u16 slot_tile(u16 s)
{
    return s < CACHE_A_SLOTS ? CACHE_A_BASE + s * 4 : CACHE_B_BASE + (s - CACHE_A_SLOTS) * 16;
}

/* remove entry h (linear probing, backward-shift deletion: no tombstones) */
static void hash_remove(u16 h)
{
    u16 j = h, k;
    for (;;) {
        j = (j + 1) & (HASH_SIZE - 1);
        if (htab[j].key == KEY_EMPTY)
            break;
        k = hslot(htab[j].key);
        /* entry j may move to h unless its home lies cyclically in (h, j] */
        if (h <= j ? (k > h && k <= j) : (k > h || k <= j))
            continue;
        htab[h] = htab[j];
        slot_ent[htab[h].slot] = h;
        h = j;
    }
    htab[h].key = KEY_EMPTY;
}

/* called by xsp_fast.s when (block, palette) is not in VRAM */
u16 cache_miss(u16 key)
{
    u16 block = key >> 4, s, n, i, start, *clk, h;
    u8 pal = key & 15;
    const SprBlock *b = &spr_blocks[block];
    const u8 *src, *lut;
    u8 *dst;
    HEnt *e;
    xsp_misses++;
    n = b->ntiles;
    if (stage_len + n * 32 > STAGE_BYTES || nup >= MAX_UP)
        return 0;
    if (n <= 4) {
        clk = &clock_a; start = 0; i = CACHE_A_SLOTS;
    } else {
        clk = &clock_b; start = CACHE_A_SLOTS; i = CACHE_B_SLOTS;
    }
    /* clock replacement: the next slot not used by the frame being built */
    for (;;) {
        s = start + *clk;
        if (++*clk >= i)
            *clk = 0;
        if (slot_ent[s] == NO_ENTRY || htab[slot_ent[s]].stamp != xframe)
            break;
        if (!--i)
            return 0;               /* every slot is in use this frame */
    }
    if (slot_ent[s] != NO_ENTRY)
        hash_remove(slot_ent[s]);
    h = hslot(key);
    while (htab[h].key != KEY_EMPTY)
        h = (h + 1) & (HASH_SIZE - 1);
    e = &htab[h];
    e->key = key;
    e->tile = slot_tile(s);
    e->stamp = xframe;
    e->slot = s;
    slot_ent[s] = h;

    src = spr_tiles + b->tile * 32;
    lut = pal_lut + ((u16)pal << 8);
    dst = stage + stage_len;
    for (i = n * 8; i; i--) {
        dst[0] = lut[src[0]]; dst[1] = lut[src[1]]; dst[2] = lut[src[2]]; dst[3] = lut[src[3]];
        dst += 4; src += 4;
    }
    up[nup].vram = e->tile * 32;
    up[nup].off = stage_len;
    up[nup].words = n * 16;
    nup++;
    stage_len += n * 32;
    return slot_tile(s);
}

/* ------------------------------------------------------------ sprite list */
const s16 size_w[16] = { 8, 8, 8, 8, 16, 16, 16, 16, 24, 24, 24, 24, 32, 32, 32, 32 };
const s16 size_h[16] = { 8, 16, 24, 32, 8, 16, 24, 32, 8, 16, 24, 32, 8, 16, 24, 32 };

void xsp_init(void)
{
    u16 i;
    for (i = 0; i < NSLOT; i++)
        slot_ent[i] = NO_ENTRY;
    for (i = 0; i < HASH_SIZE; i++)
        htab[i].key = KEY_EMPTY;
    for (i = 0; i < 66; i++)
        bhead[i] = 0xFF;
    for (i = 0; i < 16; i++)
        pal_attr[i] = 0x8000 | ((u16)pal_slot[i] << 13);
    VDP_CTRL32 = CRAM_W(2 * 32);
    for (i = 0; i < 32; i++)
        VDP_DATA = spr_cram[i];
    vdp_load_tiles(FONT_TILE_BASE, (const u32 *)text_font, 128);
}

/* xsp_set / xsp_set_obj / xobj_set: src/xsp_fast.s */

/* text rows 1.. as 8x8 sprites (row 0 is on the window plane) */
void xsp_call(s16 x, s16 y, u16 id, u16 info);

static void text_sprites(void)
{
    u8 r, c;
    for (r = 1; r < 28; r++) {
        u32 m = text_row_mask[r];
        for (c = 0; m; c++, m >>= 1)
            if (m & 1)
                xsp_call(c * 8, r * 8, 0xC000 | (FONT_TILE_BASE + (text_grid[r][c] & 0x7F)), 63);
    }
}

u16 xsp_build(HwSpr *t);
void xsp_call(s16 x, s16 y, u16 id, u16 info);

/* end of frame: front (high priority) first, keep the first 64 */
void xsp_flush(void)
{
    u16 n;
    text_sprites();
    n = xsp_build(table);
    if (!n) {
        table[0].y = 0; table[0].size = 0; table[0].attr = 0; table[0].x = 1;
        n = 1;
    }
    ntable = n;
    ncalls = 0;
    pr_lo = 64; pr_hi = 0;
    table_ready = 1;
    xframe++;
}

/* VBlank: new tiles, then the sprite table */
void xsp_upload(void)
{
    u8 i;
    for (i = 0; i < nup; i++)
        vdp_dma_vram(up[i].vram, stage + up[i].off, up[i].words);
    nup = 0;
    stage_len = 0;
    if (table_ready) {
        vdp_dma_vram(VRAM_SPRITES, table, ntable * 4);
        table_ready = 0;
    }
}
