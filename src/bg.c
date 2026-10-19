/*
 * Two-layer vertical scrolling background (port of fn_05574 / fn_056ae / fn_058de).
 *
 * The X68000 version renders 32x32 "chips" (4x4 BG tiles) into two 512x512
 * 256-colour graphic pages and scrolls them with the CRTC.  Here layer 0
 * (map.bg0, fast, foreground) is plane A and layer 1 (map.bg1, slow) is
 * plane B; both planes are 32x64 cells = 256x512 pixels, i.e. 16 chip rows,
 * the same height as a graphic page.
 */
#include "game.h"
#include "vdp.h"

typedef struct {
    s32 pos;            /* B_5933a: 24.8 fixed, decreases while scrolling */
    s32 speed;          /* B_59332 */
    s16 rowctr;         /* B_9025e >> 5: map row counter */
    s32 cam_y;          /* plane pixel at screen top (unwrapped) */
    s32 top_y;          /* plane pixel of the highest rendered chip row */
    s32 cam0;
} Layer;

s32 bg_target_speed[2];                 /* B_5932a */
static Layer layer[2];
static s16 loop_end = 0, loop_start = 0;    /* B_9046a, B_90472 */

static u16 rowbuf[2][4 * 32];
static s8 row_slot[2] = { -1, -1 };

static const u16 *const chips_of[2] = { chip_near, chip_far };
static const u8 *const map_of[2] = { map_bg0, map_bg1 };
static const u16 plane_of[2] = { VRAM_PLANE_A, VRAM_PLANE_B };

static void render_row(int l, u16 maprow, u16 *dst)
{
    const u8 *m = map_of[l] + ((maprow & 0x3FF) << 3);
    const u16 *chips = chips_of[l];
    int c, ty, tx;
    for (c = 0; c < 8; c++) {
        const u16 *ch = chips + (m[c] << 4);
        for (ty = 0; ty < 4; ty++)
            for (tx = 0; tx < 4; tx++)
                dst[ty * 32 + c * 4 + tx] = ch[ty * 4 + tx];
    }
}

/* next map row going up the screen (loop handling from fn_056ae) */
static u16 next_row(Layer *L)
{
    if (L->rowctr == loop_end)
        L->rowctr = loop_start;
    L->rowctr--;
    return (u16)L->rowctr;
}

/* fn_058de */
void bg_set_loop(s16 a, s16 b)
{
    loop_start = -(a << 4);
    loop_end = loop_start - b;
}

void bg_init(s16 a, s16 b)
{
    int l, k;
    bg_set_loop(a, b);                       /* fn_05906 */
    for (l = 0; l < 2; l++) {
        Layer *L = &layer[l];
        L->pos = 0;
        L->rowctr = loop_end;
        L->top_y = 0;
        L->cam0 = L->cam_y = 512 - 224;      /* show the bottom of the 16-row block */
        for (k = 0; k < 16; k++) {           /* fn_05574: full redraw */
            render_row(l, (u16)(L->rowctr + k), rowbuf[l]);
            vdp_load_words(plane_of[l] + k * 256, rowbuf[l], 4 * 32);
        }
        row_slot[l] = -1;
    }
    layer[0].speed = bg_target_speed[0] = -0x400;   /* fn_05998 */
    layer[1].speed = bg_target_speed[1] = -0x100;
}

void bg_update(void)
{
    int l;
    for (l = 0; l < 2; l++) {
        Layer *L = &layer[l];
        if (L->speed < bg_target_speed[l])
            L->speed++;
        if (L->speed > bg_target_speed[l])
            L->speed--;
        L->pos += L->speed;
        L->cam_y = L->cam0 + (L->pos >> 8);
        /* one chip row per frame is enough for speeds up to 32 px/frame */
        if (L->cam_y < L->top_y + 32 && row_slot[l] < 0) {
            L->top_y -= 32;
            render_row(l, next_row(L), rowbuf[l]);
            row_slot[l] = (L->top_y >> 5) & 15;
        }
    }
}

/* VBlank part: pending chip rows + vertical scroll */
void bg_upload(void)
{
    int l;
    for (l = 0; l < 2; l++) {
        if (row_slot[l] >= 0) {
            vdp_load_words(plane_of[l] + row_slot[l] * 256, rowbuf[l], 4 * 32);
            row_slot[l] = -1;
        }
    }
    vdp_vscroll((u16)layer[0].cam_y & 511, (u16)layer[1].cam_y & 511);
}
