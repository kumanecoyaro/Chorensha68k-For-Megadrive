#!/usr/bin/env python3
"""Convert Chorensha68k (X68000) data into Mega Drive data for the port.

usage: convert_assets.py SZ2.X XSP_DAT_DIR OUT_DIR

Produces OUT_DIR/assets.c and OUT_DIR/assets.h containing:
  * MD palettes 0 / 1 (far / near BG layer; 2 / 3 come from convert_sprites.py)
  * BG tiles and the chip tables (32x32 px chips as 4x4 MD name-table words)
    used when the original code draws a chip (the maps themselves are read by
    the original code, see tools/relocate.py)
"""
import os, struct, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xfile import XFile
from x68gfx import load_pal, tile8, pcg16, load_chips, load_map, chip_pixels

STAGE1_ROWS = range(700, 1024)          # map rows reached by stage 1 (row 1023 -> 766)
BG_PAL_FILE = 'bg_fb_nw.pal'

def md_rgb(c):
    return tuple(min(7, (v * 7 + 127) // 255) for v in c)

def md_word(c):
    r, g, b = c
    return (b << 9) | (g << 5) | (r << 1)

def quantize(counter, n=15):
    """Weighted k-means in 3-bit RGB space; returns list of <= n colors."""
    cols = list(counter.items())
    if len(cols) <= n:
        return [c for c, _ in sorted(cols, key=lambda t: -t[1])]
    cent = [c for c, _ in sorted(cols, key=lambda t: -t[1])[:n]]
    def d2(a, b):
        return 3 * (a[0] - b[0]) ** 2 + 4 * (a[1] - b[1]) ** 2 + 2 * (a[2] - b[2]) ** 2
    for _ in range(20):
        acc = [[0, 0, 0, 0] for _ in cent]
        for c, w in cols:
            k = min(range(len(cent)), key=lambda i: d2(c, cent[i]))
            a = acc[k]; a[0] += c[0] * w; a[1] += c[1] * w; a[2] += c[2] * w; a[3] += w
        new = []
        for a, old in zip(acc, cent):
            new.append(tuple(int(round(a[i] / a[3])) for i in range(3)) if a[3] else old)
        if new == cent:
            break
        cent = new
    return cent

class PalGroup:
    """One MD palette built from several X68000 16-color palettes."""
    def __init__(self, name):
        self.name = name
        self.count = collections.Counter()
        self.colors = None
    def add(self, rgb, w=1):
        self.count[md_rgb(rgb)] += w
    def build(self, n=15):
        self.colors = quantize(self.count, n)
        self.cache = {}
    def index(self, rgb):
        c = md_rgb(rgb)
        if c not in self.cache:
            self.cache[c] = 1 + min(range(len(self.colors)), key=lambda i:
                3 * (c[0] - self.colors[i][0]) ** 2 + 4 * (c[1] - self.colors[i][1]) ** 2 +
                2 * (c[2] - self.colors[i][2]) ** 2)
        return self.cache[c]
    def cram(self):
        cols = [(0, 0, 0)] + self.colors + [(0, 0, 0)] * (15 - len(self.colors))
        return [md_word(c) for c in cols]

def tile_bytes(px):
    """64 MD palette indices -> 32 bytes 4bpp."""
    return bytes((px[i] << 4) | px[i + 1] for i in range(0, 64, 2))

class TileSet:
    def __init__(self, base=0):
        self.tiles = []; self.index = {}; self.base = base
    def add(self, data):
        if data not in self.index:
            self.index[data] = self.base + len(self.tiles)
            self.tiles.append(data)
        return self.index[data]

# ---------------------------------------------------------------- main
def main(xpath, xsp_dir, out_dir):
    x = XFile(xpath)
    img = bytes(x.image)
    os.makedirs(out_dir, exist_ok=True)
    A = lambda n: os.path.join(xsp_dir, n)

    bgpal = load_pal(A(BG_PAL_FILE))
    sppal = load_pal(A('SZ_PALET.pal'))
    chips = load_chips(A('map.chp'))
    bgpcg = open(A('bg_pcg.sp'), 'rb').read()
    _, _, m0 = load_map(A('map.bg0'))
    _, _, m1 = load_map(A('map.bg1'))

    # ---- palettes
    g_far, g_near = PalGroup('far'), PalGroup('near')
    used0 = collections.Counter(m0[r * 8 + c] for r in STAGE1_ROWS for c in range(8))
    used1 = collections.Counter(m1[r * 8 + c] for r in STAGE1_ROWS for c in range(8))
    cpix = {}
    for ch in set(used0) | set(used1):
        cpix[ch] = chip_pixels(chips[ch], bgpcg)
    for used, g in ((used0, g_near), (used1, g_far)):
        for ch, n in used.items():
            for v in cpix[ch]:
                if v:
                    g.add(bgpal[v], n)
    g_far.build(); g_near.build()

    # sprite palettes 2 / 3 come from tools/convert_sprites.py
    g_a, g_b = PalGroup('sprA'), PalGroup('sprB')
    g_a.colors = g_b.colors = []
    groups = [g_far, g_near, g_a, g_b]

    # ---- BG tiles + chips (MD palette 0 = far layer, 1 = near layer)
    bgtiles = TileSet(base=1)            # tile 0 = blank
    def chip_words(ch, g, mdpal):
        words = []
        for wd in chips[ch]:
            pat = wd & 0xff; pal = (wd >> 8) & 15
            t = tile8(bgpcg, pat)
            px = [g.index(bgpal[pal << 4 | c]) if c else 0 for c in t]
            if not any(px):
                words.append(0); continue
            idx = bgtiles.add(tile_bytes(px))
            words.append((mdpal << 13) | (0x1000 if wd & 0x8000 else 0) | (0x800 if wd & 0x4000 else 0) | idx)
        return words
    chip_near = {ch: chip_words(ch, g_near, 1) for ch in sorted(used0)}
    chip_far = {ch: chip_words(ch, g_far, 0) for ch in sorted(used1)}
    nbg = 1 + len(bgtiles.tiles)

    # ---- one BG plane: near layer over far layer (src/bg.c).  A near tile
    # is opaque (shown as is), empty (the far tile shows) or mixed: mixed
    # tiles over each far chip are composited here, into whichever of the two
    # BG palettes fits them best (loaded into VRAM on demand by bg.c).
    def tile_px(wd):
        """chip word -> 64 X68000 colour numbers (pal*16+n, 0 = transparent), flips applied"""
        t = tile8(bgpcg, wd & 0xff); pal = (wd >> 8) & 15
        return [(pal << 4 | t[(7 - y if wd & 0x8000 else y) * 8 + (7 - x if wd & 0x4000 else x)])
                if t[(7 - y if wd & 0x8000 else y) * 8 + (7 - x if wd & 0x4000 else x)] else 0
                for y in range(8) for x in range(8)]
    def colerr(rgb, g):
        c = md_rgb(rgb); k = g.index(rgb) - 1
        q = g.colors[k]
        return 3 * (c[0] - q[0]) ** 2 + 4 * (c[1] - q[1]) ** 2 + 2 * (c[2] - q[2]) ** 2
    far_list = sorted(used1)
    far_ix = [0xFF] * 256
    for i, ch in enumerate(far_list):
        far_ix[ch] = i
    near_mix = [0] * 4096
    comp = []                                  # per mixed near tile: NF words
    comp_tiles = TileSet(base=0)
    mixed = {}
    for ch in sorted(used0):
        for i, wd in enumerate(chips[ch]):
            npx = tile_px(wd)
            nz = sum(1 for v in npx if v)
            if nz == 0 or nz == 64:
                continue
            key = (tuple(npx), i)
            if key not in mixed:
                mixed[key] = len(mixed)
                row = []
                for fch in far_list:
                    fpx = tile_px(chips[fch][i])
                    if not any(fpx[k] for k in range(64) if not npx[k]):
                        row.append(0)             # nothing of the far tile shows: the near tile alone
                        continue
                    cpx = [(npx[k], g_near) if npx[k] else (fpx[k], g_far) if fpx[k] else (0, None) for k in range(64)]
                    best = None
                    for mdpal, g in ((1, g_near), (0, g_far)):
                        err = sum(colerr(bgpal[v], g) for v, _ in cpx if v)
                        if best is None or err < best[0]:
                            best = (err, mdpal, g)
                    _, mdpal, g = best
                    t = comp_tiles.add(tile_bytes([g.index(bgpal[v]) if v else 0 for v, _ in cpx]))
                    row.append((mdpal << 13) | (t + 1))
                comp.append(row)
            near_mix[ch * 16 + i] = mixed[key] + 1
    print(f'BG merge: {len(mixed)} mixed near tiles x {len(far_list)} far chips -> {len(comp_tiles.tiles)} composite tiles')

    print(f'BG tiles {nbg}')
    font_base = 0xF000 // 32           # VRAM 0xF000-0xFFFF: text font (convert_sprites.py)

    # ---------------------------------------------------------------- write C
    def arr(ctype, name, vals, per=16, fmt='{}'):
        s = [f'const {ctype} {name}[{len(vals)}] = {{']
        for i in range(0, len(vals), per):
            s.append('  ' + ', '.join(fmt.format(v) for v in vals[i:i + per]) + ',')
        s.append('};')
        return '\n'.join(s)
    out = ['/* generated by tools/convert_assets.py from Chorensha68k data - do not edit */',
           '#include "assets.h"', '']
    out.append(arr('u16', 'pal_data', sum((g.cram() for g in groups), []), fmt='0x{:03x}'))
    alltiles = b''.join(bgtiles.tiles)
    out.append(arr('u32', 'bg_tiles', list(struct.unpack(f'>{len(alltiles)//4}I', alltiles)), 8, '0x{:08x}'))
    # chips: index by original chip number (256 entries x 16 words), unused -> 0
    cw = []
    for ch in range(256):
        cw += chip_near.get(ch, [0] * 16)
    out.append(arr('u16', 'chip_near', cw, fmt='0x{:04x}'))
    cw = []
    for ch in range(256):
        cw += chip_far.get(ch, [0] * 16)
    out.append(arr('u16', 'chip_far', cw, fmt='0x{:04x}'))
    out.append(arr('u8', 'bg_far_ix', far_ix))
    out.append(arr('u8', 'bg_near_mix', near_mix))
    out.append(arr('u16', 'bg_comp', sum(comp, []), fmt='0x{:04x}'))
    ct = b''.join(comp_tiles.tiles)
    out.append(arr('u32', 'bg_comp_tiles', list(struct.unpack(f'>{len(ct)//4}I', ct)), 8, '0x{:08x}'))
    open(os.path.join(out_dir, 'assets.c'), 'w').write('\n'.join(out) + '\n')

    h = ['/* generated by tools/convert_assets.py - do not edit */',
         '#ifndef ASSETS_H', '#define ASSETS_H', '#include "types.h"', '',
         f'#define BG_TILE_COUNT {nbg}', f'#define FONT_TILE_BASE {font_base}', '']
    h += ['', 'extern const u16 pal_data[64];',
          f'extern const u32 bg_tiles[{len(alltiles)//4}];',
          'extern const u16 chip_near[4096];', 'extern const u16 chip_far[4096];',
          f'#define BG_NFAR {len(far_list)}', f'#define BG_NMIX {len(comp)}', f'#define BG_NCOMP {len(comp_tiles.tiles)}',
          'extern const u8 bg_far_ix[256];', 'extern const u8 bg_near_mix[4096];',
          f'extern const u16 bg_comp[{len(comp) * len(far_list)}];',
          f'extern const u32 bg_comp_tiles[{len(ct)//4}];',
          '#endif']
    open(os.path.join(out_dir, 'assets.h'), 'w').write('\n'.join(h) + '\n')
    for g in groups[:2]:
        print(g.name, len(g.count), 'colors ->', len(g.colors))

if __name__ == '__main__':
    main(*sys.argv[1:4])
