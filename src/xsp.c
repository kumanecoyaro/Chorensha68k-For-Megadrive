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

/* VRAM: the stage's BG tiles (1..), the 4-tile slots, the 16-tile slots up
 * to tile 1234 (src/plane.c: the large enemy images 1234..1535).  The 4-tile
 * slots take whatever the BG set in use leaves (xsp_cache_layout): stage 1
 * 121, stage 2 65 */
#define CACHE_A_MAX   160
#ifndef CACHE_B_SLOTS
#define CACHE_B_SLOTS 40            /* (28 thrashed with stage 2's mid boss: 16-tile blocks) */
#endif
#define PAIR_SLOTS    6             /* the last 16-tile slots: laser segment pairs (pair_tile) */
#define CACHE_B_BASE  (1234 - CACHE_B_SLOTS * 16)
#define NSLOT (CACHE_A_MAX + CACHE_B_SLOTS)
static u16 cache_a_base = BG_TILE_COUNT, cache_a_n = (CACHE_B_BASE - BG_TILE_COUNT) / 4;
#define HASH_SIZE 512                   /* open addressing, key = block << 4 | palette */
#define KEY_EMPTY 0xFFFF
#define NO_ENTRY  0xFFFF

#define STAGE_BYTES 3328            /* tiles converted per frame, DMAed in VBlank (RAM: the doubler's lists) */
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
extern u8 call_next[256], bhead[128];   /* per-priority lists (src/xsp_fast.s: bhead + 128 = call_next) */
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
/* the sprite doubler (src/xsp_fast.s: xsp_build, _hint): bullets below line
 * DBL_B are written over the slots of bullets above DBL_A in the middle of
 * the frame (the build's list, the list of the frame shown) */
extern u16 dbl_n, dbl_rn, act_n;
extern volatile u16 hint_n;
extern HwSpr dbl_b[], act_b[], act_a[];
extern u8 dbl_slot[], act_slot[], dbl_on;
#define DBL_A   96                      /* (src/xsp_fast.s) */
#define DBL_MAX 40
static u8 ntable, table_ready;

/* ------------------------------------------------------------ VRAM cache */
static inline u16 hslot(u16 key)
{
    return (key ^ (key >> 7)) & (HASH_SIZE - 1);
}

static u16 slot_tile(u16 s)
{
    return s < CACHE_A_MAX ? cache_a_base + s * 4 : CACHE_B_BASE + (s - CACHE_A_MAX) * 16;
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
 * + palette, above every sprite key (block << 4 | palette) */
#define MESH_KEY  0xE000
#define MESH_MAX  64                    /* open addressing by block: block + 1, 0 = free */
_Static_assert(NBLOCK * 16 <= MESH_KEY, "sprite keys run into the meshed ones");
_Static_assert(MESH_KEY + MESH_MAX * 16 < 0xFFFF, "meshed keys run into KEY_EMPTY");
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
#ifdef TEST_KEYLOG_ALL
    if (1)
#else
    if (rom < 0 && !mesh)                   /* (debug) CPU-converted keys, read from RAM dumps */
#endif
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
            clk = &clock_a; start = 0; i = cache_a_n;
        } else {
            clk = &clock_b; start = CACHE_A_MAX; i = CACHE_B_SLOTS - PAIR_SLOTS;
        }
        /* clock replacement: the next slot not used by the frame being built
         * (an effect, loaded before the sprites: nor by the last frame, so
         * the player and the like keep their tiles) */
        for (;;) {
            u16 st;
            s = start + *clk;
            if (++*clk >= (big ? CACHE_B_SLOTS - PAIR_SLOTS : cache_a_n))
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
#define FX_ASLOTS    (cache_a_n / 2)    /* (the boss's death with the player and bullets on screen) */
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

/* ------------------------------------------------------------ PCG pairs */
/* Two calls of the same laser segment PCG with the same attributes within
 * 32x32 of each other (paired when recorded, src/xsp_fast.s h_xsp_set_boss) are
 * drawn as one 32x32 sprite: the two PCGs composited (flips applied) into one
 * of PAIR_SLOTS 16-tile slots, kept by (PCG, attributes, offset).  A
 * composite costs ~40k cycles: one a frame at most, paid for by credit that
 * the hits earn (the offsets change as the beams sweep and seldom repeat: a
 * pair without a composite is drawn as two sprites, as before).  Only while
 * a boss or a mid boss is about (xsp_pairs). */
#define PAIR_COST   64
#define PAIR_CREDIT (PAIR_COST * PAIR_SLOTS)
s16 pair_ox, pair_oy;                   /* the composite's top left from the first PCG */
static u32 pair_key[PAIR_SLOTS];        /* key + 1, 0 = free */
static u16 pair_stamp[PAIR_SLOTS];
static u16 pair_frame, pair_tick;       /* xframe of the last composite / credit */
static u16 pair_credit;                 /* + 1 a hit and every 4 frames */
#ifdef TEST_PAIRLOG
u32 pair_cnt[3];                        /* (debug) hits, composites, refused */
#endif

/* src/xsp_fast.s: a 16x16 PCG (lut converts a byte of two X68000 pixels)
 * into the 32x32 composite at dst (MD sprite tile order) at (ox, oy), flipped */
void pair_put(u8 *dst, const u8 *src, const u8 *lut, int ox, int oy, int flips);
#define NM(b) ((((b) & 0xF0) ? 0xF0 : 0) | (((b) & 0x0F) ? 0x0F : 0))
#define NM4(b) NM(b), NM(b + 1), NM(b + 2), NM(b + 3)
#define NM16(b) NM4(b), NM4(b + 4), NM4(b + 8), NM4(b + 12)
const u8 nib_mask[256] = {              /* the non-zero pixels of a byte (pair_put) */
    NM16(0), NM16(16), NM16(32), NM16(48), NM16(64), NM16(80), NM16(96), NM16(112),
    NM16(128), NM16(144), NM16(160), NM16(176), NM16(192), NM16(208), NM16(224), NM16(240)
};

u16 pair_tile(u16 ci)
{
    const Call *c = &calls[ci];
    u16 pcg = c->id & 0x3FFF, info = c->info & 0xCF00, k, best = 0;
    s16 dx = (c->x & 63) - 16, dy = (c->y & 63) - 16;
    u32 key = ((u32)pcg << 18) | ((u32)(info >> 8) << 12) | ((c->x & 63) << 6) | (c->y & 63);
    const SprBlock *b = &spr_blocks[pcg_block[pcg]];
    u8 *dst;
    pair_ox = dx < 0 ? dx : 0;
    pair_oy = dy < 0 ? dy : 0;
    for (k = 0; k < PAIR_SLOTS; k++)
        if (pair_key[k] == key + 1) {
            pair_stamp[k] = xframe;
            if (pair_credit < PAIR_CREDIT)
                pair_credit++;
#ifdef TEST_PAIRLOG
            pair_cnt[0]++;
#endif
            return CACHE_B_BASE + (CACHE_B_SLOTS - PAIR_SLOTS + k) * 16;
        }
    k = (u16)(xframe - pair_tick) >> 2;
    pair_tick += k << 2;
    pair_credit = pair_credit + k < PAIR_CREDIT ? pair_credit + k : PAIR_CREDIT;
    /* (src/xsp_fast.s LASER_LO / HI: an original info with bit 12 set is drawn as two) */
    if ((u16)(pcg - 0x50) > 0x5F - 0x50 || b->ntiles != 4
        || pair_frame == xframe || pair_credit < PAIR_COST || nup >= MAX_UP || stage_len + 512 > STAGE_BYTES) {
#ifdef TEST_PAIRLOG
        pair_cnt[2]++;
#endif
        return 0;
    }
    for (k = 1; k < PAIR_SLOTS; k++)        /* the one unused longest (not this frame's) */
        if ((u16)(xframe - pair_stamp[k]) > (u16)(xframe - pair_stamp[best]))
            best = k;
    if (pair_key[best] && pair_stamp[best] == xframe)
        return 0;
    pair_frame = xframe;
    pair_credit -= PAIR_COST;
#ifdef TEST_PAIRLOG
    pair_cnt[1]++;
#endif
    pair_key[best] = key + 1;
    pair_stamp[best] = xframe;
    dst = stage + stage_len;
    {
        u32 *z = (u32 *)dst;
        for (k = 0; k < 128; k++)
            z[k] = 0;
    }
    {
        const u8 *src = spr_tiles + b->tile * 32, *lut = pal_lut + ((info >> 8) & 15) * 256;
        pair_put(dst, src, lut, -pair_ox, -pair_oy, info | 1);
        pair_put(dst, src, lut, dx - pair_ox, dy - pair_oy, info);
    }
    k = CACHE_B_BASE + (CACHE_B_SLOTS - PAIR_SLOTS + best) * 16;
    up[nup].vram = k * 32;
    up[nup].off = stage_len;
    up[nup].words = 16 * 16;
    nup++;
    stage_len += 512;
    return k;
}

/* src/plane.c, each frame: a boss or a mid boss about: xsp_set looks for
 * laser pairs (src/xsp_fast.s h_xsp_set_boss), with credit for a composite in
 * each slot to begin with; else (the stages, the small enemies' lasers) the
 * calls go straight to h_xsp_set_main, no look at all */
void xsp_route(int on);
void xsp_pairs(int on)
{
    static u8 was;
    if (on && !was) {
        pair_credit = PAIR_CREDIT;
        pair_frame = xframe - 1;
    }
    was = on;
    xsp_route(on);
}

/* the BG set in use takes VRAM tiles 1..bg_end - 1: the 4-tile slots start
 * there; every cached block is dropped (src/bg.c, between two frames) */
void plane_fx_forget(void);
void xsp_cache_layout(u16 bg_end)
{
    u16 i;
    if (bg_end < 1)
        bg_end = 1;
    cache_a_base = bg_end;
    cache_a_n = (CACHE_B_BASE - bg_end) / 4;
    if (cache_a_n > CACHE_A_MAX)
        cache_a_n = CACHE_A_MAX;
    clock_a = clock_b = 0;
    full_frame[0] = full_frame[1] = 0xFFFF;
    for (i = 0; i < NSLOT; i++)
        slot_ent[i] = NO_ENTRY;
    for (i = 0; i < HASH_SIZE; i++)
        htab[i].key = KEY_EMPTY;
    for (i = 0; i < MESH_MAX; i++)
        mesh_blk[i] = 0;
    for (i = 0; i < PAIR_SLOTS; i++)
        pair_key[i] = 0;
    plane_fx_forget();                      /* (its held effect cells use cached tiles) */
}

/* refs src/plane.c looks for, a bit per ref: effects (expl2 / expl3 $B6-$BF,
 * flare $AA9-$AAE) and the large enemies (obj1a0 centre $B16, obj200 body $B18, crab .. obj205
 * core $B1E-$B31, obj207 $B34-$B3D, obj20A wing $B44, obj20E $B46-$B4D, obj21f $B79-$B7A); xobj_set notes their
 * calls in pl_ix */
const u8 pl_refmap[(NREF + 7) / 8] = { [22] = 0xc0, [23] = 0xff, [341] = 0x7e, [354] = 0x40, [355] = 0xc1, [356] = 0xff, [357] = 0xff, [358] = 0xf3, [359] = 0x3f, [360] = 0xf8, [361] = 0x3f, [363] = 0xc8, [364] = 0xff, [366] = 0x60, [367] = 0x06 };

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
    static u8 last_on_plane;
    if (!text_spr_dirty && !last_ntext && text_on_plane == last_on_plane)
        return;                         /* (no text sprites: the usual case in a game) */
    last_on_plane = text_on_plane;
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
u16 fstat[32][4];                           /* CPU-converted bytes, ROM bytes, calls, sprites */
extern u8 probe_n;
u16 stamp(void);
#endif
#ifdef TEST_PLCHK
/* (debug) the player ship in the built table: frames with its call, with a
 * piece not in VRAM, with a piece not in the table, without its call; the
 * last bad frame, its ncalls and sprites */
u16 plchk[8];
static void plchk_after(const Call *pc, u16 n)
{
    u16 id = pc->id, k, j, bad_c = 0, bad_t = 0;
    u8 pal = (pc->info >> 8) & 15;
    s16 px = pc->x >> 6, py = pc->y >> 6;
    plchk[0]++;
    for (k = ref_start[id]; k < ref_start[id + 1]; k++) {
        const SprPiece *q = &ref_pieces[k];
        u16 key = (q->block << 4) | ((pal ^ q->palx) & 15), h = hslot(key), tile;
        s16 sx = px + q->dx, sy = py + q->dy;
        if (sx <= -32 || sx >= 256 || sy <= -32 || sy >= 224)
            continue;
        while (htab[h].key != key && htab[h].key != KEY_EMPTY)
            h = (h + 1) & (HASH_SIZE - 1);
        if (htab[h].key != key) {
            bad_c = 1;
            continue;
        }
        tile = htab[h].tile;
        for (j = 0; j < n && (table[j].attr & 0x7FF) != tile; j++)
            ;
        if (j == n)
            bad_t = 1;
    }
    plchk[1] += bad_c;
    plchk[2] += bad_t;
    if (bad_c | bad_t) {
        plchk[4] = xframe; plchk[5] = ncalls; plchk[6] = n;
    }
}
#endif
#ifdef TEST_DROPCHK
/* (debug) enemies ($34 refs) on screen but none of their pieces in the
 * built table: per ref id (16 slots), the frames it happened */
u16 drop_id[32], drop_cnt[32], drop_seen[32], drop_frame, drop_why, drop_n, drop_ncalls;
Call drop_call;
static Call dc_call[24];
static u8 dc_n;
static void dropchk_before(void)
{
    u8 c;
    dc_n = 0;
    for (c = bhead[0x34]; c != 0xFF && dc_n < 24; c = call_next[c])
        if (!(calls[c].id & 0x8000) && calls[c].id != 0x6d)
            dc_call[dc_n++] = calls[c];
}
static void dropchk_after(u16 n)
{
    u8 k;
    for (k = 0; k < dc_n; k++) {
        const Call *pc = &dc_call[k];
        u16 id = pc->id, q, j, found = 0, vis = 0, s, why = 0;
        u8 pal = (pc->info >> 8) & 15;
        s16 px = pc->x >> 6, py = pc->y >> 6;
        for (q = ref_start[id]; q < ref_start[id + 1] && !found; q++) {
            const SprPiece *pp = &ref_pieces[q];
            u16 key = (pp->block << 4) | ((pal ^ pp->palx) & 15), h = hslot(key);
            s16 sx = px + pp->dx, sy = py + pp->dy;
            if (sx <= 0 || sx >= 240 || sy <= 0 || sy >= 208)
                continue;
            vis = 1;
            while (htab[h].key != key && htab[h].key != KEY_EMPTY)
                h = (h + 1) & (HASH_SIZE - 1);
            if (htab[h].key != key) {
                why |= 1;
                continue;
            }
            why |= 2;
            for (j = 0; j < n; j++)
                if ((table[j].attr & 0x7FF) == htab[h].tile)
                    found = 1;
        }
        if (!vis)
            continue;
        for (s = 0; s < 32 && drop_id[s] && drop_id[s] != id; s++)
            ;
        if (s == 32)
            continue;
        drop_id[s] = id;
        drop_seen[s]++;
        if (!found) {
            drop_cnt[s]++;
            drop_frame = xframe; drop_why = why; drop_n = n; drop_ncalls = ncalls; drop_call = *pc;
        }
    }
}
#endif
void xsp_flush(void)
{
    u16 n;
#ifdef TEST_PLCHK
    Call plc;
    u8 have_pl = 0;
#endif
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
#ifdef TEST_PLCHK
    {
        u8 c;
        for (c = bhead[0x3E]; c != 0xFF; c = call_next[c])
            if (!(calls[c].id & 0x8000) && calls[c].id >= 0xABD && calls[c].id < 0xAD5) {
                plc = calls[c];
                have_pl = 1;
            }
        if (!have_pl)
            plchk[3]++;
    }
#endif
#ifdef TEST_DROPCHK
    dropchk_before();
#endif
    n = xsp_build(table);
#ifdef TEST_DROPCHK
    dropchk_after(n);
#endif
#ifdef TEST_PLCHK
    if (have_pl)
        plchk_after(&plc, n);
#endif
#ifdef FRAME_PROBE
    fp[3] = stamp();
    {
        u16 *fs = fstat[probe_n & 31];
        fs[0] = stage_len; fs[1] = rom_len; fs[2] = ncalls; fs[3] = n;
    }
#endif
    if (!n) {
        table[0].y = 0; table[0].size = 0; table[0].attr = 0; table[0].x = 1;
        n = 1;
    }
    ntable = n;
    {                                   /* the doubler: as many bullets below as slots above */
        u8 k;
        static u8 hold;
        if (n + dbl_n >= MAX_HW)
            hold = 60;                  /* the doubler on while the sprites run short */
        else if (hold)
            hold--;
        dbl_on = hold != 0;
        if (dbl_n > dbl_rn) {
            /* more bullets below than bullet slots above: any entry done by
             * line DBL_A (bottom above it) takes one */
            const HwSpr *t = table;
            u8 j;
            for (k = 0, j = 0; j < n; j++, t++)  /* (bottom: y + 8 * (cells - 1) + 8 - 128) */
                if (t->y + (s16)((t->size >> 5) & 0x18) <= DBL_A + 128 - 8) {
                    dbl_slot[k++] = j;
                    if (k == DBL_MAX)
                        break;
                }
            if (k > dbl_rn)
                dbl_rn = k;
        }
        if (dbl_n > dbl_rn)
            dbl_n = dbl_rn;
        for (k = 0; k < dbl_n; k++)
            dbl_b[k].size = (dbl_b[k].size & 0x0F00) | (table[dbl_slot[k]].size & 0x7F);
    }
    ncalls = 0;
    pl_n = 0;
    fx_stage = fx_aslots = fx_bslots = 0;
    base_valid = 0;
    pr_lo = 64; pr_hi = 0;
    table_ready = 1;
    xframe++;
}

void dma_rom(u16 vram, const u32 *src, u16 words);
void xsp_lag(void);

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
        u8 k;
        vdp_dma_vram(VRAM_SPRITES, table, ntable * 4);
        table_ready = 0;
        act_n = dbl_n;                      /* the doubler for this table (the H-int) */
        for (k = 0; k < act_n; k++) {
            act_slot[k] = dbl_slot[k];
            act_b[k] = dbl_b[k];
            act_a[k] = table[dbl_slot[k]];
        }
        hint_n = act_n;
    } else
        xsp_lag();
}

/* VBlank without a new sprite table: the bullets above the doubler's line
 * back into their slots (the H-int wrote the ones below over them) */
void xsp_lag(void)
{
    u8 k;
    for (k = 0; k < act_n; k++) {
        const u16 *w = (const u16 *)&act_a[k];
        VDP_CTRL32 = VRAM_W(VRAM_SPRITES + act_slot[k] * 8);
        VDP_DATA = w[0]; VDP_DATA = w[1]; VDP_DATA = w[2]; VDP_DATA = w[3];
    }
    hint_n = act_n;
}
