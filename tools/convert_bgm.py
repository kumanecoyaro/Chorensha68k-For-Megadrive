#!/usr/bin/env python3
"""Prepare Chorensha68k MDX songs (YM2151, 8 FM channels) for the Mega Drive driver.

usage: convert_bgm.py SZ2_BGM.CNF BGM_DIR OUT_DIR

The sequence data stays MDX (src/music.c interprets it at run time); this
script only does the chip dependent part:

  * voices keep the parameters both chips share: ALG, FB, DT1, MUL, TL, KS, AR,
    AM enable, D1R, D2R, D1L, RR (DT2 is dropped; the YM2151 hardware LFO, $FE
    direct register writes and noise frequency details are ignored by the driver)
  * the YM2612 has 6 FM channels, so per song two MDX channels are muted:
    channels that layer another one (unison / octave doubling, echo copies),
    else chord voices that mostly double the other parts (see choose_dropped).
    The kept partner of a stereo-spread pair is panned centre.  YM2151 noise
    notes are played as rests.  (The driver can also send channels to the
    DCSG: kind $10/$11.)
  * pitch tables: F-number for the YM2612 (7.67 MHz) and DCSG periods (3.58 MHz)
    in 1/64 semitone steps; Timer-B tempo -> ticks per frame.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mdx import MDX, Track, CARRIERS

FPS = 53693175 / 3420 / 262              # NTSC field rate, ~59.92 Hz
YM_CLOCK = 53693175 / 7
PSG_CLOCK = 53693175 / 15

def track_stats(mdx):
    """Per channel: [(tick, note, pan)] of keyed notes, noise note count, pans used."""
    trs = [Track(mdx, i) for i in range(min(8, len(mdx.tracks)))]
    notes = [[] for _ in trs]; noise = [0] * len(trs); pans = [{3} for _ in trs]
    for t in range(40000):
        for i, tr in enumerate(trs):
            for e in tr.step(t, lambda v: None):
                if e[0] == 'note' and not e[4]:
                    notes[i].append((t, e[1], tr.pan))
                    if trs[7].noise is not None and trs[7].noise & 0x80 and i == 7:
                        noise[i] += 1
                elif e[0] == 'opm' and e[1] == 0x0F:
                    trs[7].noise = e[2]                 # OPM noise: channel H only
                elif e[0] == 'pan':
                    pans[i].add(e[1])
        if all(tr.ended or tr.loop_pos is not None for tr in trs):
            break
    return notes, noise, pans

def doubling(a, b):
    """a copying b (same onset, unison or octaves) at the best delay 0-48 ticks:
    (fraction of a's notes, delay, fraction of those panned differently)."""
    best = (0, 0, 0)
    if not a or not b:
        return best
    for d in range(49):
        on = {t + d: (n, p) for t, n, p in b}
        m = [(p, on[t][1]) for t, n, p in a if t in on and (n - on[t][0]) % 12 == 0 and abs(n - on[t][0]) <= 24]
        if len(m) > best[0]:
            best = (len(m), d, sum(1 for p, q in m if p != q))
    return best[0] / len(a), best[1], best[2] / max(1, best[0])

def choose_dropped(mdx):
    """Pick the two MDX channels that are not played on the YM2612.

    Unused channels and channels that only double another one (unison / octave
    layering, echo copies) go first - the copied part still sounds.  If a song
    has fewer than two, the channel that most often doubles a note of the
    remaining ones (chord voices in unison) follows.
    Returns (dropped, fm6, center): fm6 is the kept channel that goes on FM6,
    which the DAC takes over while a sound effect plays - the one with the
    smallest pitch range (drums / pedal bass), so the melody is never cut by
    shots; center = kept channels whose stereo partner was dropped."""
    notes, noise, pans = track_stats(mdx)
    n = len(notes)
    drop = [i for i in range(n) if not notes[i]][:2]
    center = set()
    cands = []
    for i in range(n):
        for j in range(n):
            if i != j and notes[i] and notes[j]:
                f, d, pd = doubling(notes[i], notes[j])
                if f >= 0.7:
                    cands.append((f, d == 0 and i > j, i, j, d, pd))
    cands.sort(reverse=True)            # best copies first; of a mutual unison pair drop the later channel
    copies = []; partial = []
    for f, _, i, j, d, pd in cands:
        if len(drop) < 2 and i not in drop and j not in drop and all(i != s for _, s, _, _ in copies):
            drop.append(i); copies.append((i, j, f, d))
            if pd > 0.25:                               # stereo spread: keep the part in the middle
                center.add(j)
    while len(drop) < 2:
        # partial layering: the channel whose notes most often sound the same
        # note (or its octave) at the same time on another kept channel (chord voices
        # doubling each other); a copied channel is kept for its dropped copy
        sources = {j for _, j, _, _ in copies}
        live = [k for k in range(n) if k not in drop]
        score = []
        for i in live:
            if i in sources:
                continue
            oth = [{t: x for t, x, _ in notes[k]} for k in live if k != i]
            r = sum(1 for t, x, _ in notes[i]
                    if any(t in o and abs(o[t] - x) in (0, 12) for o in oth)) / len(notes[i])
            score.append((r + (0.05 if noise[i] else 0), i))
        r, i = max(score)
        drop.append(i); partial.append((i, r))
    drop.sort()
    kept = [i for i in range(8) if i not in drop]
    rng = lambda i: (max(x[1] for x in notes[i]) - min(x[1] for x in notes[i]) if notes[i] else -1,
                     len(notes[i]), -i)
    fm6 = min(kept, key=rng)
    center -= set(drop)
    return drop, fm6, center, copies, partial

DT2_RATIO = (1.0, 1.41, 1.57, 1.73)

def dt2_mul(dt1mul, dt2):
    """YM2151 DT2 multiplies an operator's frequency by 1.41 / 1.57 / 1.73
    (the inharmonic partials of the drum voices); the YM2612 has no DT2: the
    nearest MUL (0 = x0.5) instead."""
    mul = dt1mul & 15
    if dt2:
        r = (mul or 0.5) * DT2_RATIO[dt2]
        mul = 0 if r < 0.75 else min(15, int(r + 0.5))
    return (dt1mul & 0x70) | mul

def voice_bytes(v):
    """28 bytes: no, FB/ALG, slot mask, carrier mask, then 4 x (DT1/MUL, TL, KS/AR, AM/D1R, D2R, D1L/RR)."""
    cm = sum(1 << i for i in CARRIERS[v.con])
    out = [v.no, (v.fb << 3) | v.con, v.mask, cm]
    out += [dt2_mul(x, d >> 6) for x, d in zip(v.dt1mul, v.dt2d2r)]
    out += [x & 0x7F for x in v.tl]
    out += [x & 0xDF for x in v.ksar]
    out += [x & 0x9F for x in v.amed1r]
    out += [x & 0x1F for x in v.dt2d2r]          # DT2 (bits 7-6): folded into MUL above
    out += list(v.d1lrr)
    return out

def parse_cnf(path):
    text = open(path, 'rb').read().decode('cp932')
    return [(int(m.group(1), 16), os.path.basename(m.group(2)))
            for m in re.finditer(r'^\s*BGM_No\.\$([0-9A-Fa-f]+)\s*:\s*FILE\s*=\s*(\S+\.MDC)', text, re.M)]

def arr(ctype, name, vals, per=16, static=True):
    s = [f'{"static " if static else ""}const {ctype} {name}[{len(vals)}] = {{']
    for i in range(0, len(vals), per):
        s.append('  ' + ','.join(str(v) for v in vals[i:i + per]) + ',')
    s.append('};')
    return s

def main(cnf, bgmdir, out):
    os.makedirs(out, exist_ok=True)
    ents = parse_cnf(cnf)
    c = ['/* generated by tools/convert_bgm.py - do not edit */', '#include "bgm.h"', '']
    songs = {}; total = 0
    for no, mdc in ents:
        name = mdc[:-4]
        if name in songs:
            continue
        mdx = MDX(open(os.path.join(bgmdir, name + '.MDX'), 'rb').read())
        drop, fm6, center, copies, partial = choose_dropped(mdx)
        vb = []
        for vno in sorted(mdx.voices):
            vb += voice_bytes(mdx.voices[vno])
        body = list(mdx.body)
        c += arr('u8', f'body_{name}', body, 32)
        c += arr('u8', f'voices_{name}', vb, 28)
        kinds = []
        fm = 0
        for i in range(8):
            if i in drop:
                kinds.append(0xFF)
            elif i == fm6:
                kinds.append(5)
            else:
                kinds.append(fm); fm += 1
        trk = ', '.join(f'body_{name} + {mdx.tracks[i]}' for i in range(8))
        c.append(f'static const Song song_{name} = {{ {{ {trk} }}, voices_{name}, {len(mdx.voices)}, '
                 f'{{ {", ".join(str(k) for k in kinds)} }}, {sum(1 << i for i in center)} }};')
        size = len(body) + len(vb) + 48
        songs[name] = size; total += size
        cp = ' '.join([f'{"ABCDEFGH"[i]}={"ABCDEFGH"[j]}+{d}({f:.2f})' for i, j, f, d in copies] +
                      [f'{"ABCDEFGH"[i]}~({r:.2f})' for i, r in partial])
        print(f'{name:8s} muted ch {"".join("ABCDEFGH"[i] for i in drop)}  FM6 {"ABCDEFGH"[fm6]}  '
              f'doubles: {cp or "-":28s} centre {"".join("ABCDEFGH"[i] for i in sorted(center)) or "-"}  {size} bytes')
    c.append('const Song *const bgm_table[256] = {')
    for no, mdc in ents:
        c.append(f'  [0x{no:02X}] = &song_{mdc[:-4]},')
    c.append('};')
    # pitch tables, 1/64 semitone resolution over one octave
    fn = []
    for r in range(768):
        f = 440.0 * 2 ** ((24 + r / 64 - 69) / 12)          # MIDI 24 (C1) .. at block 1
        fn.append(int(round(f * 144 * 2 ** 20 / YM_CLOCK)))
    ps = []
    for r in range(768):
        f = 440.0 * 2 ** ((r / 64 - 69) / 12)                # MIDI 0.. ; shift right by octave
        ps.append(min(65535, int(round(PSG_CLOCK / (32 * f)))))
    tpf = []
    for tb in range(256):
        tpf.append(int(round(65536 * 4000000 / (1024 * (256 - tb)) / FPS)))
    c += arr('u16', 'fnum_tab', fn, static=False)
    c += arr('u16', 'psg_tab', ps, static=False)
    c += arr('u32', 'tempo_tab', tpf, static=False)
    open(os.path.join(out, 'bgm.c'), 'w').write('\n'.join(c) + '\n')
    open(os.path.join(out, 'bgm.h'), 'w').write('\n'.join([
        '/* generated by tools/convert_bgm.py - do not edit */',
        '#ifndef BGM_H', '#define BGM_H', '#include "types.h"',
        '/* kind: 0-5 = YM2612 FM channel, $10/$11 = DCSG voice 0/1, $FF = muted',
        '   center: bit n = MDX channel n always panned centre (its doubling partner is muted) */',
        'typedef struct { const u8 *track[8]; const u8 *voices; u8 nvoices; u8 kind[8]; u8 center; } Song;',
        'extern const Song *const bgm_table[256];',
        'extern const u16 fnum_tab[768];      /* F-number, MIDI 24 + n/64 at block 1 */',
        'extern const u16 psg_tab[768];       /* DCSG period, MIDI n/64 (>> octave) */',
        'extern const u32 tempo_tab[256];     /* Timer-B value -> ticks per frame (16.16) */',
        f'#define BGM_DATA_SIZE {total}', '#endif', '']))
    print(f'{len(songs)} songs, {total} bytes (+ {768 * 4 + 1024} bytes of tables)')

if __name__ == '__main__':
    main(*sys.argv[1:4])
