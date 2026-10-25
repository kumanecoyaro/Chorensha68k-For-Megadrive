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

#define CACHE_A_BASE  BG_TILE_COUNT   /* VRAM tile of the first 4-tile slot (after the BG tiles) */
#define CACHE_A_SLOTS 96
#define CACHE_B_BASE  (CACHE_A_BASE + CACHE_A_SLOTS * 4)
#define CACHE_B_SLOTS 28            /* (ends at tile 1234: src/plane.c tiles 1234..1535) */
_Static_assert(CACHE_B_BASE + CACHE_B_SLOTS * 16 <= 1234, "sprite cache overlaps the plane A enemy tiles");
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
static u16 full_frame[2] = { 0xFFFF, 0xFFFF };  /* xframe at which the 4 / 16-tile slots ran out */
u16 xsp_misses, xsp_fail;
#ifdef TEST_FAILLOG
u16 fail_up, fail_full[2], fail_key[16], fail_n;   /* (debug) why cache_miss refused, which keys */
#endif

static u8 stage[STAGE_BYTES] __attribute__((aligned(4)));
static u16 stage_len;
#define ROM_BYTES 4096              /* tiles DMAed from ROM per frame (VBlank time) */
static u16 rom_len;
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

/* meshed copies of effect blocks (xsp_tile_for): keys MESH_KEY + index * 16
 * + palette, above every sprite key (block < 2788: key < $AE50) */
#define MESH_KEY  0xB000
#define MESH_MAX  64                    /* open addressing by block: block + 1, 0 = free */
static u16 mesh_blk[MESH_MAX];

#ifdef TEST_KEYLOG
u16 keylog[256], keylog_n;
#endif
static u8 fx_loading;                   /* cache_miss for a plane A effect (xsp_tile_for) */
/* called by xsp_fast.s when (block, palette) is not in VRAM */
u16 cache_miss(u16 key)
{
    u16 block = key >> 4, s, n, i, start, *clk, h;
    u8 pal = key & 15, mesh = key >= MESH_KEY;
    if (mesh)
        block = mesh_blk[(key - MESH_KEY) >> 4] - 1;
    const SprBlock *b = &spr_blocks[block];
    const u8 *src, *lut;
    u8 *dst;
    HEnt *e;
    s16 rom = -1;                           /* pre-converted copy in ROM: pre_tiles tile */
    xsp_misses++;
    n = b->ntiles;
    {                                       /* (convert_sprites.py) */
        const u16 *keys = mesh ? mesh_keys : pre_keys;
        u16 lo = 0, nk = mesh ? MESH_NKEYS : PRE_NKEYS, hi = nk, k = (block << 4) | pal;
        while (lo < hi) {
            u16 m = (lo + hi) >> 1;
            if (keys[m] < k) lo = m + 1; else hi = m;
        }
        if (lo < nk && keys[lo] == k)
            rom = (mesh ? mesh_off : pre_off)[lo];
    }
#ifdef TEST_KEYLOG
    if (rom < 0 && !mesh)                   /* (debug) CPU-converted keys, read from RAM dumps */
        keylog[keylog_n++ & 255] = key;
#endif
    if (nup >= MAX_UP || (rom >= 0 ? rom_len + n * 32 > ROM_BYTES : stage_len + n * 32 > STAGE_BYTES)) {
        xsp_fail++;
#ifdef TEST_FAILLOG
        fail_up++; fail_key[fail_n++ & 15] = key;
#endif
        return 0;
    }
    {
        u8 big = n > 4;
        if (full_frame[big] == xframe) {
            xsp_fail++;
#ifdef TEST_FAILLOG
            fail_full[big]++; fail_key[fail_n++ & 15] = key;
#endif
            return 0;               /* (found full already this frame) */
        }
        if (!big) {
            clk = &clock_a; start = 0; i = CACHE_A_SLOTS;
        } else {
            clk = &clock_b; start = CACHE_A_SLOTS; i = CACHE_B_SLOTS;
        }
        /* clock replacement: the next slot not used by the frame being built
         * (an effect, loaded before the sprites: nor by the last frame, so
         * the player and the like keep their tiles) */
        for (;;) {
            u16 st;
            s = start + *clk;
            if (++*clk >= (big ? CACHE_B_SLOTS : CACHE_A_SLOTS))
                *clk = 0;
            if (slot_ent[s] == NO_ENTRY)
                break;
            st = htab[slot_ent[s]].stamp;
            if (st != xframe && !(fx_loading && st == (u16)(xframe - 1)))
                break;
            if (!--i) {
                if (fx_loading)
                    return 0;               /* (the sprites may still find one) */
                full_frame[big] = xframe;
                xsp_fail++;
#ifdef TEST_FAILLOG
                fail_full[big]++; fail_key[fail_n++ & 15] = key;
#endif
                return 0;           /* every slot is in use this frame */
            }
        }
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

    if (rom >= 0) {                         /* DMA straight from ROM */
        up[nup].vram = e->tile * 32;
        up[nup].off = 0x8000 | rom;
        up[nup].words = n * 16;
        nup++;
        rom_len += n * 32;
        return slot_tile(s);
    }
    src = spr_tiles + b->tile * 32;
    lut = pal_lut + ((u16)pal << 8);
    dst = stage + stage_len;
    for (i = n * 8; i; i--) {               /* a byte: 2 pixels, a long: a tile row */
        dst[0] = lut[src[0]]; dst[1] = lut[src[1]]; dst[2] = lut[src[2]]; dst[3] = lut[src[3]];
        if (mesh)                           /* checkerboard: every other pixel, offset by a row */
            *(u32 *)dst &= i & 1 ? 0x0F0F0F0FUL : 0xF0F0F0F0UL;
        dst += 4; src += 4;
    }
    up[nup].vram = e->tile * 32;
    up[nup].off = stage_len;
    up[nup].words = n * 16;
    nup++;
    stage_len += n * 32;
    return slot_tile(s);
}

/* VRAM tile of (block, palette) for an effect drawn into plane A
 * (src/plane.c), loading it if needed; mesh: a checkerboard copy (half the
 * pixels: the X68000's translucent bomb).  Effects get a share of the
 * frame's tile uploads and of the 4 / 16-tile slots so they never starve the
 * sprites (built after them); 0 = over budget, not drawn this frame. */
#define FX_STAGE_MAX 1536
#ifndef FX_ROM_MAX
#define FX_ROM_MAX   3072               /* the meshed bomb: DMA from ROM, no conversion */
#endif
#define FX_ASLOTS    40                 /* (of 96: the boss's death with the player and bullets) */
#define FX_BSLOTS    12
static u16 fx_stage, fx_aslots, fx_bslots;
u16 xsp_tile_for(u16 block, u8 pal, u8 mesh, u16 *keyp)
{
    u16 key = (block << 4) | pal, h, n = spr_blocks[block].ntiles, t;
    if (mesh) {
        for (t = block & (MESH_MAX - 1); mesh_blk[t] != block + 1; t = (t + 1) & (MESH_MAX - 1))
            if (!mesh_blk[t]) {
                mesh_blk[t] = block + 1;
                break;
            }
        key = MESH_KEY + (t << 4) + pal;
    }
    *keyp = key;
    if (n > 4 ? fx_bslots >= FX_BSLOTS : fx_aslots >= FX_ASLOTS)
        return 0;
    h = hslot(key);
    while (htab[h].key != KEY_EMPTY) {
        if (htab[h].key == key) {
            if (htab[h].stamp != xframe) {
                if (n > 4)
                    fx_bslots++;
                else
                    fx_aslots++;
            }
            htab[h].stamp = xframe;
            return htab[h].tile;
        }
        h = (h + 1) & (HASH_SIZE - 1);
    }
    if (fx_stage + n * 32 > (mesh ? FX_ROM_MAX : FX_STAGE_MAX))
        return 0;
    fx_loading = 1;
    t = cache_miss(key);
    fx_loading = 0;
    if (t) {
        fx_stage += n * 32;
        if (n > 4)
            fx_bslots++;
        else
            fx_aslots++;
    }
    return t;
}

/* an effect tile still shown by plane A this frame (src/plane.c): keep it */
void xsp_touch(u16 key)
{
    u16 h = hslot(key);
    while (htab[h].key != KEY_EMPTY) {
        if (htab[h].key == key) {
            htab[h].stamp = xframe;
            return;
        }
        h = (h + 1) & (HASH_SIZE - 1);
    }
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
        pal_attr[i] = (u16)pal_slot[i] << 13;   /* low priority: under the text (plane A, window) */
    VDP_CTRL32 = CRAM_W(2 * 32);
    for (i = 0; i < 32; i++)
        VDP_DATA = spr_cram[i];
    vdp_load_tiles(FONT_TILE_BASE, (const u32 *)text_font, 128);
}

/* xsp_set / xsp_set_obj / xobj_set: src/xsp_fast.s */

/* text rows 1.. as 8x8 sprites (row 0 is on the window plane) */
void xsp_call(s16 x, s16 y, u16 id, u16 info);

static u8 last_ntext;                   /* text sprites at the front of the last table */

extern u8 text_on_plane;                /* (plane.c shows the text rows) */
extern u8 text_map[28];                 /* screen row r shows text row text_map[r] (x68.c) */
static void text_sprites(void)
{
    u8 r, c, n = 0;
    for (r = text_on_plane ? 28 : 1; r < 28; r++) {
        u8 tr = text_map[r];
        u32 m = text_row_mask[tr];
        for (c = 0; m; c++, m >>= 1)
            if (m & 1) {
                xsp_call(c * 8, r * 8, 0xC000 | (FONT_TILE_BASE + (text_grid[tr][c] & 0x7F)), 63);
                n++;
            }
    }
    last_ntext = n < MAX_HW ? n : MAX_HW;
    text_spr_dirty = 0;
}

/* x_wait (src/x68.c): the last table with its text sprites (its first
 * entries: the very front) replaced by the current text.  The other sprites
 * of the last flush are kept aside on the first refresh (in the tail of the
 * tile staging buffer, unused until the next flush), so text that does not
 * leave them room does not lose them for good. */
static u8 base_n, base_valid;
#define BASE ((HwSpr *)(stage + STAGE_BYTES - MAX_HW * sizeof(HwSpr)))
void xsp_text_refresh(void)
{
    u8 r, c, n = 0, rest, i;
    if (!base_valid) {                  /* the last table's sprites but the text (high priority) */
        base_n = 0;
        for (i = 0; i < ntable; i++)
            if (!(table[i].attr & 0x8000) && (table[i].y || table[i].x != 1))
                BASE[base_n++] = table[i];
        base_valid = 1;
    }
    for (r = text_on_plane ? 28 : 1; r < 28; r++) {
        u32 m = text_row_mask[text_map[r]];
        for (; m; m &= m - 1)
            n++;
    }
    if (n > MAX_HW)
        n = MAX_HW;
    rest = base_n;
    if (n + rest > MAX_HW)
        rest = MAX_HW - n;
    for (i = 0; i < rest; i++)
        table[n + i] = BASE[i];
    i = 0;
    for (r = 1; r < 28 && i < n; r++) {
        u8 tr = text_map[r];
        u32 m = text_row_mask[tr];
        for (c = 0; m && i < n; c++, m >>= 1)
            if (m & 1) {
                table[i].y = r * 8 + 128;
                table[i].size = 0;      /* 1x1 */
                table[i].attr = 0x8000 | (TEXT_PAL << 13) | (FONT_TILE_BASE + (text_grid[tr][c] & 0x7F));
                table[i].x = c * 8 + 128;
                i++;
            }
    }
    n = n + rest;
    if (!n) {
        table[0].y = 0; table[0].attr = 0; table[0].x = 1;
        n = 1;
    }
    for (i = 0; i < n; i++)
        table[i].size = (table[i].size & 0x0F00) | (i + 1 < n ? i + 1 : 0);
    ntable = n;
    last_ntext = n - rest;
    table_ready = 1;
    text_spr_dirty = 0;
}

u16 xsp_build(HwSpr *t);
void xsp_call(s16 x, s16 y, u16 id, u16 info);

/* end of frame: front (high priority) first, keep the first 64 */
void plane_frame(void);
extern u16 pl_n;

#ifdef FRAME_PROBE
u16 fprobe[32][4];                          /* plane_frame / text_sprites / xsp_build */
extern u8 probe_n;
u16 stamp(void);
#endif
void xsp_flush(void)
{
    u16 n;
#ifdef FRAME_PROBE
    u16 *fp = fprobe[probe_n & 31];
    fp[0] = stamp();
#endif
    plane_frame();                      /* (may drop the calls of a large enemy) */
#ifdef FRAME_PROBE
    fp[1] = stamp();
#endif
    text_sprites();
#ifdef FRAME_PROBE
    fp[2] = stamp();
#endif
    n = xsp_build(table);
#ifdef FRAME_PROBE
    fp[3] = stamp();
#endif
    if (!n) {
        table[0].y = 0; table[0].size = 0; table[0].attr = 0; table[0].x = 1;
        n = 1;
    }
    ntable = n;
    ncalls = 0;
    pl_n = 0;
    fx_stage = fx_aslots = fx_bslots = 0;
    base_valid = 0;
    pr_lo = 64; pr_hi = 0;
    table_ready = 1;
    xframe++;
}

void dma_rom(u16 vram, const u32 *src, u16 words);

/* VBlank: new tiles, then the sprite table */
void xsp_upload(void)
{
    u8 i;
    for (i = 0; i < nup; i++)
        if (up[i].off & 0x8000)             /* pre-converted tiles in ROM */
            dma_rom(up[i].vram, pre_tiles + (up[i].off & 0x7FFF) * 8, up[i].words);
        else
            vdp_dma_vram(up[i].vram, stage + up[i].off, up[i].words);
    nup = 0;
    stage_len = 0;
    rom_len = 0;
    if (table_ready) {
        vdp_dma_vram(VRAM_SPRITES, table, ntable * 4);
        table_ready = 0;
    }
}
