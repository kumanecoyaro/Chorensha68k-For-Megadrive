/*
 * MDX music driver (replaces MCDRV / MXDRV, YM2151 -> YM2612 + DCSG).
 *
 * The MDX sequence data is played as is.  Six MDX channels drive YM2612 FM
 * channels; the other two are muted (kind $FF; the DCSG code path for
 * kind $10/$11 is kept but not used for now).  Only the parameters
 * both FM chips share are used; YM2151-only features (DT2, hardware LFO,
 * direct register writes other than the noise switch) are ignored.
 *
 * Ticks follow the MDX Timer-B tempo; music_update() runs once per frame and
 * executes as many ticks as have elapsed (tempo_tab, 8.8 fixed point).
 */
#include "game.h"
#include "bgm.h"
#include "vdp.h"

#define NCH 8
#define MUTED 0xFF

typedef struct {
    u8 w;               /* 0 saw, 1 square, 2 triangle (+4: fine resolution, same here) */
    u16 period, t;
    s32 step, val;      /* 1/256 units */
    s8 dir;
    u8 first;
} Lfo;

typedef struct {
    const u8 *pos;
    u16 wait;
    u8 ended;
    const u8 *voice;    /* 28-byte converted voice */
    u8 vol;             /* v 0-15 */
    s16 atvol;          /* @v: attenuation 0 (loudest) - 127, -1 = use v */
    u8 pan;
    u8 q;
    s16 detune;         /* 1/64 semitone */
    s16 porta;          /* 1/256 of detune unit per tick */
    u8 legato;          /* $F7 seen: the next note is not keyed off at its end */
    u8 tie;             /* previous note was tied: this note only changes pitch */
    u8 kdelay;
    u8 mp_w, ma_w, mp_on, ma_on;
    u16 mp_p, ma_p;
    s16 mp_a, ma_a;
    u8 noise, nfreq;
    u8 sp;
    struct { u8 cnt; const u8 *pos; } stk[8];
    /* output */
    u8 kind;            /* 0-5 FM channel, $10/$11 DCSG voice, $FF muted */
    u8 on;
    u16 gate;
    u8 delay;
    s16 note;           /* -1: none yet */
    s32 pacc;
    Lfo mp, ma;
    u8 mp_act, ma_act;
    u8 used_noise;
} MCh;

/* (v >> 8) of a 32-bit field; volatile so gcc can't narrow it into a word
 * load at an odd address (68000 address error) */
static s32 shr8(const s32 *p)
{
    return *(const volatile s32 *)p >> 8;
}

static const u8 vol_table[16] = {
    0x2A, 0x28, 0x25, 0x22, 0x20, 0x1D, 0x1A, 0x18, 0x15, 0x12, 0x10, 0x0D, 0x0A, 0x08, 0x05, 0x02
};
static const u8 key_code[6] = { 0, 1, 2, 4, 5, 6 };

static const Song *song;
static MCh ch[NCH];
static u8 tempo;
static u16 tick_acc;
static u8 playing;
static u16 last_vbl;

void ym_write(u8 part, u8 reg, u8 val);
void psg_write(u8 v);

static void fm_reg(u8 c, u8 reg, u8 val)
{
    ym_write(c >= 3, reg + (c % 3), val);
}

static const u8 *find_voice(u8 no)
{
    const u8 *v = song->voices;
    u8 i;
    for (i = 0; i < song->nvoices; i++, v += 28)
        if (v[0] == no)
            return v;
    return NULL;
}

static s16 atten(const MCh *c)
{
    return c->atvol >= 0 ? c->atvol : vol_table[c->vol];
}

static void fm_tl(MCh *c, s16 lfo)
{
    const u8 *v = c->voice;
    s16 a = atten(c) + lfo;
    u8 i;
    if (!v || c->kind == MUTED)
        return;
    for (i = 0; i < 4; i++) {
        s16 tl = v[8 + i];
        if (v[3] & (1 << i)) {
            tl += a;
            if (tl < 0) tl = 0;
            if (tl > 127) tl = 127;
        }
        fm_reg(c->kind, 0x40 + i * 4, (u8)tl);
    }
}

static void fm_voice(MCh *c)
{
    const u8 *v = c->voice;
    u8 i;
    if (!v || c->kind == MUTED)
        return;
    for (i = 0; i < 4; i++) {
        u8 o = i * 4;
        fm_reg(c->kind, 0x30 + o, v[4 + i]);
        fm_reg(c->kind, 0x50 + o, v[12 + i]);
        fm_reg(c->kind, 0x60 + o, v[16 + i]);
        fm_reg(c->kind, 0x70 + o, v[20 + i]);
        fm_reg(c->kind, 0x80 + o, v[24 + i]);
        fm_reg(c->kind, 0x90 + o, 0);
    }
    fm_reg(c->kind, 0xB0, v[1]);
    fm_tl(c, 0);
}

static void psg_vol(MCh *c, u8 on)
{
    u8 v = c->kind & 1, a = 15;
    if (on) {
        s16 x = atten(c) + (c->ma_act ? (s16)shr8(&c->ma.val) : 0);
        x = (x * 3) >> 3;
        a = x < 0 ? 0 : x > 15 ? 15 : x;
    }
    if (c->noise) {
        c->used_noise = 1;
        psg_write(0xF0 | a);
        psg_write(0x90 | (v << 5) | 15);
    } else {
        psg_write(0x90 | (v << 5) | a);
        if (c->used_noise)
            psg_write(0xFF);
    }
}

static void set_pitch(MCh *c)
{
    s32 p = (s32)(c->note + 15) * 64 + c->detune + shr8(&c->pacc);
    u16 oct, rem;
    if (c->kind == MUTED)
        return;
    if (c->mp_act)
        p += shr8(&c->mp.val);
    if (p < 0)
        p = 0;
    oct = (u16)p / 768;
    rem = (u16)p % 768;
    if (c->kind < 0x10) {
        u16 fn = fnum_tab[rem];
        s16 blk = oct - 1;
        if (blk < 0) { fn >>= 1; blk = 0; }
        if (blk > 7) blk = 7;
        fm_reg(c->kind, 0xA4, (blk << 3) | (fn >> 8));
        fm_reg(c->kind, 0xA0, fn & 0xFF);
    } else if (!c->noise) {
        u16 n = psg_tab[rem] >> oct;
        u8 v = c->kind & 1;
        while (n > 1023)
            n >>= 1;
        if (!n)
            n = 1;
        psg_write(0x80 | (v << 5) | (n & 15));
        psg_write((n >> 4) & 0x3F);
    }
}

static void key_on(MCh *c)
{
    c->on = 1;
    if (c->kind == MUTED)
        return;
#ifdef MUSIC_SOLO               /* debug: only one MDX channel audible */
    if (c != &ch[MUSIC_SOLO])
        return;
#endif
    if (c->kind < 0x10) {
        ym_write(0, 0x28, ((c->voice ? c->voice[2] : 15) << 4) | key_code[c->kind]);
    } else {
        if (c->noise) {
            u8 rate = c->nfreq >= 24 ? 0 : c->nfreq >= 12 ? 1 : 2;
            psg_write(0xE4 | rate);
        }
        psg_vol(c, 1);
    }
}

static void key_off(MCh *c)
{
    if (!c->on)
        return;
    c->on = 0;
    if (c->kind == MUTED)
        return;
    if (c->kind < 0x10)
        ym_write(0, 0x28, key_code[c->kind]);
    else
        psg_vol(c, 0);
}

static void lfo_reset(Lfo *l, u8 w, u16 p, s16 a)
{
    l->w = w;
    l->period = p ? p : 1;
    l->step = a;        /* change per tick in 1/256 detune (MP) or TL (MA) units */
    l->t = 0;
    l->dir = 1;
    l->first = 1;
    l->val = 0;
    if ((w & 3) == 0)
        l->val = -(l->step * (s32)(l->period >> 1));
}

static void lfo_tick(Lfo *l)
{
    l->t++;
    switch (l->w & 3) {
    case 0:
        l->val += l->step;
        if (l->t >= l->period) {
            l->t = 0;
            l->val = -(l->step * (s32)(l->period >> 1));
        }
        break;
    case 1:
        if (l->t >= l->period) {
            l->t = 0;
            l->dir = -l->dir;
        }
        l->val = l->dir > 0 ? l->step : -l->step;
        break;
    default:
        l->val += l->dir > 0 ? l->step : -l->step;
        if (l->t >= (l->first ? l->period >> 1 : l->period)) {
            l->t = 0;
            l->dir = -l->dir;
            l->first = 0;
        }
        break;
    }
}

/* big-endian word at any address: the 68000 can't load words from odd
 * addresses, and gcc would merge two plain byte loads into one */
static s16 rd16(const u8 *p)
{
    const volatile u8 *v = p;
    return (s16)((v[0] << 8) | v[1]);
}

/* interpret commands until the next note / rest (Track.step in tools/mdx.py) */
static void step(MCh *c)
{
    u16 guard = 0;
    if (c->ended)
        return;
    if (c->wait) {
        c->wait--;
        return;
    }
    while (++guard < 1000) {
        u8 cmd = *c->pos++;
        if (cmd <= 0x7F) {                          /* rest */
            key_off(c);
            c->wait = cmd;
            return;
        }
        if (cmd <= 0xDF) {                          /* note */
            u16 len = *c->pos++ + 1, gate;
            u8 tied = c->tie && c->on;
            /* gate: q1-7 = len*q/8; q8 = until the next note (MXDRV keys off
             * right before the next key-on); @q (>8) = len - (256-q) ticks;
             * a note after $F7 is never keyed off by the gate */
            if (c->legato || c->q == 8) {
                gate = 0;
            } else if (c->q < 8) {
                gate = (u16)(len * c->q) >> 3;
                if (!gate)
                    gate = 1;
            } else {
                s16 g = len - (256 - c->q);
                gate = g < 1 ? 1 : g;
            }
            c->tie = c->legato;
            c->legato = 0;
            if (!tied)
                key_off(c);
            c->note = cmd - 0x80;
            if (!tied) {
                c->pacc = 0;
                c->mp_act = c->mp_on;
                if (c->mp_act)
                    lfo_reset(&c->mp, c->mp_w, c->mp_p, c->mp_a);
                c->ma_act = c->ma_on;
                if (c->ma_act)
                    lfo_reset(&c->ma, c->ma_w, c->ma_p, c->ma_a);
            }
            set_pitch(c);
            if (c->kind < 0x10 && !tied)
                fm_tl(c, 0);
            c->gate = gate;
            if (!tied) {
                c->delay = c->kdelay;
                if (c->delay == 0)
                    key_on(c);
            }
            c->wait = len - 1;
            return;
        }
        switch (cmd) {
        case 0xFF: tempo = *c->pos++; break;
        case 0xFE:                                  /* OPM register: only the noise switch */
            if (c->pos[0] == 0x0F) {
                c->noise = c->pos[1] >> 7;
                c->nfreq = c->pos[1] & 31;
            }
            c->pos += 2;
            break;
        case 0xFD:
            c->voice = find_voice(*c->pos++);
            if (c->kind < 0x10)
                fm_voice(c);
            break;
        case 0xFC:
            c->pan = *c->pos++ & 3;
            if (c->kind < 0x10) {
                static const u8 pans[4] = { 0x00, 0x80, 0x40, 0xC0 };
                fm_reg(c->kind, 0xB4, pans[c->pan]);
            }
            break;
        case 0xFB: {
            u8 v = *c->pos++;
            if (v & 0x80) c->atvol = v & 0x7F;
            else { c->vol = v & 15; c->atvol = -1; }
            goto vol_changed;
        }
        case 0xFA:                                  /* ( volume down */
            if (c->atvol >= 0) { if (c->atvol < 127) c->atvol++; }
            else if (c->vol > 0) c->vol--;
            goto vol_changed;
        case 0xF9:                                  /* ) volume up */
            if (c->atvol >= 0) { if (c->atvol > 0) c->atvol--; }
            else if (c->vol < 15) c->vol++;
        vol_changed:
            if (c->kind < 0x10)
                fm_tl(c, 0);
            else if (c->on && c->kind != MUTED)
                psg_vol(c, 1);
            break;
        case 0xF8: c->q = *c->pos++; break;
        case 0xF7: c->legato = 1; break;
        case 0xF6:
            if (c->sp < 8) {
                c->stk[c->sp].cnt = c->pos[0];
                c->stk[c->sp].pos = c->pos + 2;
                c->sp++;
            }
            c->pos += 2;
            break;
        case 0xF5:
            c->pos += 2;
            if (c->sp) {
                if (--c->stk[c->sp - 1].cnt)
                    c->pos = c->stk[c->sp - 1].pos;
                else
                    c->sp--;
            }
            break;
        case 0xF4: {
            s16 off = rd16(c->pos);
            c->pos += 2;
            if (c->sp && c->stk[c->sp - 1].cnt == 1) {
                c->sp--;
                c->pos += off;
            }
            break;
        }
        case 0xF3:
            c->detune = rd16(c->pos);
            c->pos += 2;
            if (c->note >= 0)
                set_pitch(c);
            break;
        case 0xF2: c->porta = rd16(c->pos); c->pos += 2; break;
        case 0xF1:
            if (c->pos[0] == 0) {
                c->ended = 1;
                key_off(c);
                return;
            }
            c->pos += 2 + rd16(c->pos);             /* loop */
            break;
        case 0xF0: c->kdelay = *c->pos++; break;
        case 0xEF: c->pos++; break;                 /* sync send: ignored */
        case 0xEE: break;                           /* sync wait: ignored */
        case 0xED:
            c->noise = *c->pos >> 7;
            c->nfreq = *c->pos & 31;
            c->pos++;
            break;
        case 0xEC: {
            u8 w = *c->pos++;
            if (w == 0x80) c->mp_on = 0;
            else if (w == 0x81) c->mp_on = c->mp_p != 0;
            else {
                c->mp_w = w; c->mp_p = (u16)rd16(c->pos); c->mp_a = rd16(c->pos + 2);
                c->pos += 4; c->mp_on = 1;
            }
            break;
        }
        case 0xEB: {
            u8 w = *c->pos++;
            if (w == 0x80) c->ma_on = 0;
            else if (w == 0x81) c->ma_on = c->ma_p != 0;
            else {
                c->ma_w = w; c->ma_p = (u16)rd16(c->pos); c->ma_a = rd16(c->pos + 2);
                c->pos += 4; c->ma_on = 1;
            }
            break;
        }
        case 0xEA:                                  /* YM2151 hardware LFO: ignored */
            if (*c->pos++ < 0x80)
                c->pos += 4;
            break;
        case 0xE9: c->pos++; break;                 /* LFO delay: ignored */
        case 0xE8: break;
        case 0xE7:
            if (*c->pos++ == 1)
                c->pos++;
            break;
        default:
            c->ended = 1;                           /* unknown command */
            key_off(c);
            return;
        }
    }
    c->ended = 1;
}

static void tick(void)
{
    u8 i;
    for (i = 0; i < NCH; i++) {
        MCh *c = &ch[i];
        step(c);
        if (c->note < 0)
            continue;
        if (c->delay) {
            if (--c->delay == 0)
                key_on(c);
        }
        if (c->on || c->delay) {
            u8 changed = 0;
            if (c->gate && --c->gate == 0)
                key_off(c);
            if (c->porta) {
                c->pacc += c->porta;
                changed = 1;
            }
            if (c->mp_act) {
                lfo_tick(&c->mp);
                changed = 1;
            }
            if (changed)
                set_pitch(c);
            if (c->ma_act) {
                lfo_tick(&c->ma);
                if (c->kind < 0x10)
                    fm_tl(c, (s16)shr8(&c->ma.val));
                else if (c->on && c->kind != MUTED)
                    psg_vol(c, 1);
            }
        }
    }
}

void music_stop(void)
{
    u8 i;
    playing = 0;
    for (i = 0; i < 6; i++)
        ym_write(0, 0x28, key_code[i]);
    for (i = 0; i < 4; i++)
        psg_write(0x9F | (i << 5));
}

/* fn_03590: start BGM number `no` (SZ2_BGM.CNF numbering) */
void music_play(u8 no)
{
    u8 i, j;
    music_stop();
    song = bgm_table[no];
    if (!song)
        return;
    ym_write(0, 0x22, 0);                           /* YM2612 LFO off */
    ym_write(0, 0x27, 0);                           /* ch3 normal, timers off */
    for (i = 0; i < NCH; i++) {
        MCh *c = &ch[i];
        u8 *p = (u8 *)c;
        for (j = 0; j < sizeof(MCh); j++)
            p[j] = 0;
        c->pos = song->track[i];
        c->kind = song->kind[i];
        c->vol = 8;
        c->atvol = -1;
        c->q = 8;
        c->note = -1;
        c->pan = 3;
        if (c->kind < 0x10)
            fm_reg(c->kind, 0xB4, 0xC0);
    }
    tempo = 200;
    tick_acc = 0;
    last_vbl = vblank_count;
    playing = 1;
}

/* advance by the number of VBlanks since the last call, so slow game
 * frames don't slow the music down */
void music_update(void)
{
    u16 n = vblank_count - last_vbl;
    last_vbl += n;
    if (!playing)
        return;
    while (n--) {
        tick_acc += tempo_tab[tempo];
        while (tick_acc >= 256) {
            tick_acc -= 256;
            tick();
        }
    }
}
