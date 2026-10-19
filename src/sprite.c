/* Hardware sprite list builder: XSP style composite frames -> MD sprite table */
#include "game.h"
#include "vdp.h"

#define MAX_SPR 64          /* H32 mode limit */
#define LAYER_MAX 48

typedef struct { u16 y, size, attr, x; } HwSpr;

static HwSpr layer_buf[LAYER_COUNT][LAYER_MAX];
static u8 layer_n[LAYER_COUNT];
static HwSpr table[MAX_SPR];
static u8 table_n;

void spr_begin(void)
{
    int i;
    for (i = 0; i < LAYER_COUNT; i++)
        layer_n[i] = 0;
}

static void put(u8 layer, s16 sx, s16 sy, u8 size, u16 attr)
{
    HwSpr *h;
    s16 w = ((size >> 2) + 1) * 8, ht = ((size & 3) + 1) * 8;
    /* clip: MD shows screen x 0..255, y 0..223; hardware x 0 would mask */
    if (sx <= -w || sx >= 256 || sy <= -ht || sy >= 224)
        return;
    if (sx + 128 <= 0)
        return;
    if (layer_n[layer] >= LAYER_MAX)
        return;
    h = &layer_buf[layer][layer_n[layer]++];
    h->y = sy + 128;
    h->size = (u16)size << 8;
    h->attr = attr;
    h->x = sx + 128;
}

/* draw composite frame centred like XSP: centre pixel = (x >> 6) - 8 */
void spr_frame(u8 layer, u16 frame, s16 x, s16 y)
{
    u16 i, e = frame_start[frame + 1];
    s16 cx = (x >> 6) - 8, cy = (y >> 6) - 8;
    for (i = frame_start[frame]; i < e; i++) {
        const SprPiece *p = &frame_pieces[i];
        put(layer, cx + p->dx, cy + p->dy, p->size, p->attr);
    }
}

void spr_text(u8 layer, s16 px, s16 py, const char *s)
{
    static const char chars[] = FONT_CHARS;
    for (; *s; s++, px += 8) {
        u16 k;
        for (k = 0; chars[k] && chars[k] != *s; k++)
            ;
        if (k && chars[k])
            put(layer, px, py, 0, 0x8000 | (2 << 13) | (FONT_TILE_BASE + k));
    }
}

void spr_end(void)
{
    int l, i;
    table_n = 0;
    for (l = 0; l < LAYER_COUNT; l++)
        for (i = 0; i < layer_n[l] && table_n < MAX_SPR; i++)
            table[table_n++] = layer_buf[l][i];
    if (table_n == 0) {
        table[0].y = 0; table[0].size = 0; table[0].attr = 0; table[0].x = 1;
        table_n = 1;
    }
    for (i = 0; i < table_n; i++)
        table[i].size = (table[i].size & 0x0F00) | (i + 1 < table_n ? i + 1 : 0);
}

void spr_upload(void)
{
    vdp_load_words(VRAM_SPRITES, (const u16 *)table, table_n * 4);
}
