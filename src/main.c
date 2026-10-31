/* Chorensha68k Mega Drive port: boot, frame hooks, top level (L03e60) */
#include "game.h"
#include "vdp.h"
#include "orig.h"
#include "spr.h"

/* the original's RAM must end below the port's (src/md.ld: $FF9DC0) */
_Static_assert(ORIG_RAM + ORIG_RAM_SIZE <= 0xFF9DC0, "original RAM overlaps the port's");

long ocall(void *fn, long a1, long a2, long a3, long a4);
void text_map_set(u8 kind);
long x_bg_load(void);
long x_text_clear(void);
void plane_upload(void);
void plane_act(void);
void sound_dma(u8 on);
void bg_chips(void);
void tb_vblank(void);

u16 frame_count;
/* the score panel (window plane, screen cells 32..39 on the right, H40):
 * the game's text rows 0..3 (the score line, the bombs, the ships) laid out
 * in 8 columns, in opaque glyphs (the panel covers the sprites running off
 * the play area: they are low priority) put in the unused part of the window
 * table (src/vdp.h) */
static const char hud_chars[] = " 0123456789SCOREHI*\x04\x06\x07";
#define HUD_ATTR (0x8000 | (TEXT_PAL << 13))

static u16 hud_tile(u8 i)               /* glyph i (< 22): +$20 / +$60 of window row i / 2 */
{
    return (VRAM_WINDOW + (i >> 1) * 128 + ((i & 1) ? 0x60 : 0x20)) / 32;
}

/* character -> the cell (computed once: hud_init) */
static const u8 hud_map[128] = { ['0'] = 1, ['1'] = 2, ['2'] = 3, ['3'] = 4, ['4'] = 5, ['5'] = 6, ['6'] = 7, ['7'] = 8, ['8'] = 9, ['9'] = 10, ['S'] = 11, ['C'] = 12, ['O'] = 13, ['R'] = 14, ['E'] = 15, ['H'] = 16, ['I'] = 17, ['*'] = 18, [0x04] = 19, [0x06] = 20, [0x07] = 21 };   /* (hud_chars) */
static u16 hud_cell(u8 c)
{
    return HUD_ATTR | hud_tile(hud_map[c & 127]);
}

static void hud_row(u8 r, const u8 *s, u8 n)    /* panel row r: n characters (the rest blank) */
{
    u8 i;
    u16 b = hud_cell(' ');
    VDP_CTRL32 = VRAM_W(VRAM_WINDOW + r * 128 + 64);
    for (i = 0; i < 8; i++)
        VDP_DATA = i < n ? hud_cell(s[i]) : b;
}

static void hud_init(void)
{
    u8 i, r, k;
    for (i = 0; hud_chars[i]; i++) {    /* the glyphs, background colour 2 (black) */
        const u32 *t = &text_font[(u8)hud_chars[i] * 8];
        VDP_CTRL32 = VRAM_W(hud_tile(i) * 32);
        for (r = 0; r < 8; r++) {
            u32 v = t[r], o = 0;
            for (k = 0; k < 8; k++) {
                u32 p = (v >> (k * 4)) & 15;
                o |= (p ? p : 2) << (k * 4);
            }
            VDP_DATA32 = o;
        }
    }
    for (r = 0; r < 28; r++)
        hud_row(r, (const u8 *)"", 0);
}

/* the text rows the panel shows in a game (its score line in row 0): 0..3,
 * the text sprites / plane A text from row text_row0() on */
u8 text_row0(void)
{
    return text_grid[0][1] == 'S' && text_grid[0][2] == 'C' ? 4 : 0;
}

/* HI SCORE / SCORE on top, the ships below them (text row 1: 8 to a row,
 * the game gives 10 and more), the bombs (rows 2..3, 2 cells tall, 5 at
 * most) at the bottom */
static void hud_build(void)
{
    static const u8 score[] = "SCORE", hi[] = "HI SCORE";
    static const u8 ships[] = "\x04\x04\x04\x04\x04\x04\x04\x04";
    static u8 l_game = 0xFF, l_n = 0xFF, l_b = 0xFF, l_hi[8], l_sc[8];
    u8 game = text_row0() != 0, n = 0, b = 0, r, top[8], bot[8], i;
    const u8 *sh = &text_grid[0][23], *sc = &text_grid[0][4];
    u8 all = game != l_game;
    l_game = game;
    if (all) {
        hud_row(1, hi, game ? 8 : 0);
        hud_row(4, score, game ? 5 : 0);
    }
    for (i = 0; i < 8 && sh[i] == l_hi[i]; i++)
        ;
    if (all || i < 8) {
        hud_row(2, sh, game ? 8 : 0);
        for (i = 0; i < 8; i++)
            l_hi[i] = sh[i];
    }
    for (i = 0; i < 8 && sc[i] == l_sc[i]; i++)
        ;
    if (all || i < 8) {
        hud_row(5, sc, game ? 8 : 0);
        for (i = 0; i < 8; i++)
            l_sc[i] = sc[i];
    }
    if (game) {
        while (n < 31 && text_grid[1][1 + n] == 0x04)
            n++;
        while (b < 7 && text_grid[2][1 + b] == 0x06)
            b++;
    }
#ifdef HUD_TEST
    n = HUD_TEST;
#endif
    if (OW(0x4a104))                    /* invincible (the title's up + START): bit 3 */
        b |= 8;
    if (all || n != l_n) {
        for (r = 0; r < 18; r++) {
            u8 k = n > r * 8 ? (n - r * 8 > 8 ? 8 : n - r * 8) : 0;
            u8 lk = l_n != 0xFF && l_n > r * 8 ? (l_n - r * 8 > 8 ? 8 : l_n - r * 8) : 0;
            if (all || k != lk)
                hud_row(7 + r, ships, k);
        }
        l_n = n;
    }
    if (all || b != l_b) {
        for (i = 0; i < 8; i++) {
            top[i] = i < (b & 7) ? 0x06 : ' ';
            bot[i] = i < (b & 7) ? 0x07 : ' ';
        }
        if (b & 8)
            top[7] = '*';
        hud_row(26, top, 8);
        hud_row(27, bot, 8);
        l_b = b;
    }
}

/* called by the vsync hook at the start of VBlank */
/* VBlank interrupt: show the finished frame (sprites, new tiles, BG chips,
 * scroll, score line), then the sound.  The game loop only hands over
 * complete frames (frame_ready), so a slow frame simply shows the previous
 * one again, and music / sound effects keep their own 60 Hz pace. */
volatile u8 frame_ready;
void xsp_lag(void);
void plane_vblank(void);
static u8 vbl_busy;

#ifdef FRAME_PROBE
u16 vprobe[32][4];
u16 stamp(void);
#endif
void vblank_work(void)
{
#ifdef FRAME_PROBE
    u16 *vp = vprobe[vblank_count & 31];
    vp[0] = stamp();
#endif
    if (vbl_busy)
        return;
    vbl_busy = 1;
    /* first the scrolls and the H-int of the frame shown (while still in the
     * VBlank: the H-int counter takes register 10 only there; set later, after
     * the DMA ran into the display, the first H-int came anywhere and the
     * band switches of src/plane.c went late), then the DMA */
    if (frame_ready) {
        sound_dma(1);
        xsp_upload();
#ifdef PLANE_ONLY
        VDP_CTRL32 = VRAM_W(VRAM_SPRITES);
        VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0;
#endif
        sound_dma(0);
        plane_act();
    } else
        xsp_lag();
    plane_vblank();                     /* plane A scroll, the H-int for the frame shown */
    tb_vblank();                        /* (the text in plane B: the screen off, plane A busy) */
    bg_chips();
    if (frame_ready) {
        sound_dma(1);
        bg_upload();
        plane_upload();
        sound_dma(0);
        if (text_dirty) {
            hud_build();
            text_dirty = 0;
        }
        frame_ready = 0;
    }
    /* the VDP work is done: the H-int may come in during the rest (the
     * sound, which can run into the display: level 4 let in, else the band
     * switches of src/plane.c came late and the rows below shook) */
    __asm__ volatile("move.w #0x2300, %sr");
    input_poll();
#ifdef FRAME_PROBE
    vp[1] = stamp();
#endif
#ifndef NO_MUSIC
    bgm_service();
    music_update();
#endif
#ifdef FRAME_PROBE
    vp[2] = stamp();
#endif
    sound_update();
#ifdef FRAME_PROBE
    vp[3] = stamp();
#endif
    frame_count++;
    vbl_busy = 0;
}

/* fn_05224 hook: graphic palette registers -> BG palettes (static for now) */
void pal_from_orig(void)
{
}

int main(void)
{
    vdp_init();
    vdp_set_palettes(pal_data, 64);
    vdp_load_tiles(1, bg_sets[0].tiles, bg_sets[0].ntiles);
    sound_init();
    xsp_init();
    hud_init();
    text_map_set(0);
    vdp_display(1);
    __asm__ volatile("move.w #0x2300, %sr");      /* (H-int: the sprite doubler) */

    x_bg_load();                            /* fn_053ba (start-up file loading) */

    /* L03e60: option defaults, the title menu (fn_02dac) and the games it
     * starts (patched by tools/relocate.py: "exit" and the idle replay demo
     * stay on the title), forever */
    ocall(OFN(0x03e60), 0, 0, 0, 0);
    return 0;
}
