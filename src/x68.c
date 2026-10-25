/*
 * Mega Drive replacements for the X68000 library calls made by the original
 * game code (hooked by tools/relocate.py, see src/hooks.s).
 *
 * Arguments come from XC-compiled callers: every argument is pushed as a
 * long, so the C prototypes take longs and narrow them.
 */
#include "game.h"
#include "vdp.h"
#include "orig.h"

long ocall(void *fn, long a1, long a2, long a3, long a4);

u16 pad_now, pad_prev;
static u16 autofire;

long x_nop(void)
{
    return 0;
}

/* ------------------------------------------------------------------ input */

/* X68000 joystick byte (IOCS _JOYGET): active low, bit 0 up, 1 down, 2 left,
 * 3 right, 5 trigger A, 6 trigger B.  MD: A = shot, B = bomb, C = rapid
 * shot (MD addition: the game wants one press per shot), START = pause
 * (the original pauses on left+right). */
static u8 joy_byte(void)
{
    u8 v = 0xFF;
    u16 p = pad_now;
    if (p & PAD_UP) v &= ~0x01;
    if (p & PAD_DOWN) v &= ~0x02;
    if (p & PAD_LEFT) v &= ~0x04;
    if (p & PAD_RIGHT) v &= ~0x08;
    if (p & PAD_A) v &= ~0x20;
    if ((p & PAD_C) && autofire < 3) v &= ~0x20;
    if (p & PAD_B) v &= ~0x40;
    if (p & PAD_START) v &= ~0x0C;
    return v;
}

/* fn_39e7a: IOCS _JOYGET(port) */
long x_joyget(long port)
{
    return port == 0 ? joy_byte() : 0xFF;
}

/* fn_39e16: numeric keypad direction (5 = centre -> 0) for port 1/2 */
long x_joydir(long port)
{
    u8 j;
    s16 d = 0;
    if (port != 1)
        return port == 2 ? 0 : -1;
    j = joy_byte();
    if (!(j & 1)) d += 3;
    if (!(j & 2)) d -= 3;
    if (!(j & 4)) d -= 1;
    if (!(j & 8)) d += 1;
    if (d)
        d += 5;
    return d;
}

/* fn_39e58: trigger bits (1 = A, 2 = B) */
long x_joybtn(long port)
{
    if (port != 1)
        return port == 2 ? 0 : -1;
    return (~joy_byte() >> 5) & 3;
}

/* fn_39dac: IOCS _BITSNS (keyboard matrix): no keyboard */
long x_bitsns(long group)
{
    (void)group;
    return 0;
}

/* fn_00f26: direction for the player; mode != 0 = replay playback (unused) */
long x_input_dir(long mode)
{
    (void)mode;
    return x_joydir(1);
}

/* fn_01048: trigger bits for the player (B_4a10c swaps A/B) */
long x_input_btn(long mode)
{
    u16 b = x_joybtn(1);
    (void)mode;
    if (OW(0x4a10c))
        b = ((b << 1) & 2) | ((b >> 1) & 1);
    return b;
}

void input_poll(void)
{
    pad_prev = pad_now;
    pad_now = pad_read();
    if (++autofire >= 6)
        autofire = 0;
}

/* ------------------------------------------------------------------ frame */
extern volatile u8 frame_ready;

/* fn_2cbda: the original waits for the vsync after the last sprite output.
 * Here the wait is in x_xsp_out, just before the next frame's sprites are
 * built: the game logic of a frame runs while the last one waits for its
 * VBlank, so a frame slightly over 1/60 s no longer costs a whole frame
 * (30 fps) but only what it overruns. */
#ifdef FRAME_PROBE
u16 probe[32][4];               /* per frame: line stamps at vsync end, xsp_out, after flush */
u8 probe_n;
u16 stamp(void)
{
    u8 v = *(volatile u8 *)0xC00008;
    return (u16)(vblank_count * 262 + (v < 0xE0 ? v + 40 : v - 0xE0));
}
#define PROBE(k) probe[probe_n & 31][k] = stamp()
#else
#define PROBE(k)
#endif

long x_vsync(long n)
{
    long waited = 0;
    (void)n;
#ifdef TEST_MUTEKI
    OW(0x4a104) = 1;                /* original debug option: no death (the title resets it) */
#endif
#ifdef TEST_CRAB_HP1
    {                               /* stage 1 crabs (obj202) die at the first hit: obj21f */
        u16 k;
        if (OW(0x59328) == 0x118)
            for (k = 0; k < 0x17F; k++)
                if (OW(0x4d39e + k * 0x80 + 0x22) == 0x202 && OW(0x4d39e + k * 0x80 + 0x5c) > 1)
                    OW(0x4d39e + k * 0x80 + 0x5c) = 1;
    }
#endif
    bg_stage_check();
    waited = frame_ready;
    PROBE(0);
    return waited;
}

/* frame skip: game frame k is due at VBlank sched.  When the game falls two
 * VBlanks behind, one frame's sprites (and plane A, scroll) are not built:
 * the screen keeps the last frame for one more VBlank and the game catches
 * up (SKIP_MAX in a row at most; past 4 VBlanks behind the debt is
 * forgiven: the game slows down instead) */
#ifndef SKIP_MAX
#define SKIP_MAX 2                  /* frames skipped in a row at most (20 fps at worst) */
#endif
static u16 sched;
static u8 skipped;
u16 frames_skipped;
void xsp_skip(void);

/* fn_2cf3a: XSP display list done; p -> {x0, x1, y0, y1} BG scroll (24.8) */
long x_xsp_out(long p)
{
    const volatile s32 *s = (const volatile s32 *)p;
    s16 debt = (s16)(vblank_count - sched);
    PROBE(1);
    if (debt < 0)
        debt = 0, sched = vblank_count;
    else if (debt > 4)
        debt = 4, sched = vblank_count - 4;
    sched++;
    if (debt >= 2 && skipped < SKIP_MAX) {
        skipped++;
        frames_skipped++;
        xsp_skip();
        PROBE(2);
#ifdef FRAME_PROBE
        probe_n++;
#endif
        return 1;
    }
    skipped = 0;
    while (frame_ready)             /* the last frame is still waiting for its VBlank */
        ;
    xsp_flush();
    PROBE(2);
#ifdef FRAME_PROBE
    probe_n++;
#endif
    bg_scroll(s[0] >> 8, s[1] >> 8, s[2] >> 8, s[3] >> 8);
    frame_ready = 1;
    return 1;
}

/* fn_2cf40: the same without BG scroll (title screen) */
long x_xsp_out0(void)
{
    while (frame_ready)
        ;
    xsp_flush();
    frame_ready = 1;
    return 1;
}

/* fn_2ff62(n): wait n + 1 vertical blanks (title menu, game over, pause,
 * ranking).  The original's text screen shows at once; here the text rows
 * below the score line are sprites, so text changed meanwhile is put into
 * the last frame's sprite list. */
void xsp_text_refresh(void);
u8 plane_text_refresh(void);
long x_wait(long n)
{
    u16 i;
    for (i = 0; i <= (u16)n; i++) {
        while (frame_ready)
            ;
        if (text_spr_dirty) {
            plane_text_refresh();       /* (plane A when free, else sprites) */
            xsp_text_refresh();
        }
        frame_ready = 1;
        while (frame_ready)
            ;
    }
    return 0;
}

/* fn_3975a (IOCS $B2): graphic screen display, d1 = pages shown (0: off,
 * 3: both).  The original turns it off, draws the whole stage screen and
 * turns it on (game start), so the first frame shows a complete screen.
 * Off: both planes cleared (what is drawn next is a new screen) and plane B
 * hidden until it is turned on again. */
void bg_reset(void);
void bg_show(u8 on);
void plane_reset(void);
long x_screen_clear(long mode)
{
    u16 i;
    while (frame_ready)
        ;
    bg_show(mode != 0);
    if (mode)
        return 0;
    bg_reset();
    plane_reset();
    for (i = 0; i < 16; i++) {          /* planes A ($C000) and B ($E000), 4 KB each */
        __asm__ volatile("move.w #0x2700, %sr");
        vdp_fill(VRAM_PLANE_A + i * 256, 0, 128);
        vdp_fill(VRAM_PLANE_B + i * 256, 0, 128);
        __asm__ volatile("move.w #0x2500, %sr");
    }
    return 0;
}

/* ------------------------------------------------------------------ text */
u8 text_grid[32][32];
u8 text_row_n[32];              /* non-blank characters per row */
u32 text_row_mask[32];          /* bit c = column c holds a character */
u8 text_dirty;
u8 text_spr_dirty;              /* a row shown by sprites (1..) changed since the last flush */
u8 text_pl_dirty;               /* a row 1.. changed since plane A last showed the text */

/* fn_2f06a(x, y, ch) */
long x_text_putc(long x, long y, long ch)
{
    u8 *p = &text_grid[y & 31][x & 31];
    u8 c = (u8)ch == ' ' ? 0 : (u8)ch;
    if (*p != c) {
        text_row_n[y & 31] += (c != 0) - (*p != 0);
        if (c)
            text_row_mask[y & 31] |= 1UL << (x & 31);
        else
            text_row_mask[y & 31] &= ~(1UL << (x & 31));
        *p = c;
        text_dirty = 1;
        if (y & 31)
            text_spr_dirty = text_pl_dirty = 1;
    }
    return 0;
}

/* fn_2f2aa: clear the text screen */
long x_text_clear(void)
{
    u8 *p = &text_grid[0][0];
    u16 i;
    for (i = 0; i < 32 * 32; i++)
        p[i] = 0;
    for (i = 0; i < 32; i++) {
        text_row_n[i] = 0;
        text_row_mask[i] = 0;
    }
    text_dirty = 1;
    text_spr_dirty = text_pl_dirty = 1;
    return 0;
}

/* ------------------------------------------------------------------ sound */
long x_se_request(long no)
{
    sound_request((u8)no);
    return 0;
}

/* music requests are carried out by the VBlank interrupt, which owns the YM2612 */
static volatile s16 bgm_req = -1;       /* -1 none, $100 stop, else BGM number */

long x_bgm_play(long no)
{
    bgm_req = (u8)no;
    return 0;
}

long x_bgm_fade(long speed)
{
    (void)speed;
    bgm_req = 0x100;
    return 0;
}

void bgm_service(void)
{
    s16 r = bgm_req;
    if (r < 0)
        return;
    bgm_req = -1;
    if (r == 0x100)
        music_stop();
    else
        music_play((u8)r);
}

/* ------------------------------------------------------------------ misc */
/* screen row r (1-27) shows text row text_map[r]: the X68000's text screen
 * has 32 rows, the Mega Drive shows 28 (the score line + 27) */
u8 text_map[28];
void text_map_set(u8 kind)
{
    u8 r;
    for (r = 0; r < 28; r++)
        text_map[r] = r;
    if (kind == 1) {                    /* ranking / name entry (fn_0132c): rows 1, 4, 6-30 */
        text_map[2] = 4;
        for (r = 3; r < 28; r++)
            text_map[r] = r + 3;
    } else if (kind == 2) {             /* title ranking (fn_011ce at row 10): rows 2-28 */
        for (r = 1; r < 28; r++)
            text_map[r] = r + 1;
    }
    text_spr_dirty = text_pl_dirty = 1;
}

/* fn_0132c(score, stage): the original's ranking and name entry */
long orig_0132c(void);
long x_game_over(long score, long stage)
{
    long r;
    text_map_set(1);
    r = ocall(orig_0132c, score, stage, 0, 0);
    text_map_set(0);
    return r;
}

long x_pal_upload(void)
{
    pal_from_orig();
    return 0;
}

long x_pal_anim(void)
{
    return 0;
}

long x_pal_load(void)
{
    return 0;
}
