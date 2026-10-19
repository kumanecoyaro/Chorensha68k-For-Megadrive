#!/usr/bin/env python3
"""Convert Chorensha68k (X68000) data into Mega Drive data for the port.

usage: convert_assets.py SZ2.X XSP_DAT_DIR OUT_DIR

Produces OUT_DIR/assets.c and OUT_DIR/assets.h containing:
  * 4 MD palettes (CRAM words)
  * BG tiles, chip table (32x32 px chips as 4x4 MD name-table words), maps bg0/bg1
  * sprite frames (lists of MD hardware sprite pieces) + sprite tiles
  * tables lifted from SZ2.X: trig table (speed 0..15 x 256 angles), atan table,
    random table, shot power table
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
    def build(self):
        self.colors = quantize(self.count)
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

# ---------------------------------------------------------------- sprites
class SpriteSrc:
    def __init__(self, xsp_dir):
        self.d = xsp_dir
        self.cache = {}
    def file(self, name):
        if name not in self.cache:
            self.cache[name] = open(os.path.join(self.d, name), 'rb').read()
        return self.cache[name]
    def ref_pixels(self, name, idx, xsp=None):
        """Composite one XSP ref (object) frame -> {(x,y): color} relative to object center."""
        ref = self.file(name + '.ref'); frm = self.file(name + '.frm')
        data = self.file((xsp or name) + '.xsp')
        c, p = struct.unpack_from('>hI', ref, idx * 8)
        ax = ay = 0; pix = {}
        for j in range(c):
            vx, vy, pt, rv = struct.unpack_from('>hhhH', frm, p + j * 8)
            ax += vx; ay += vy
            px = pcg16(data, pt)
            for y in range(16):
                for x in range(16):
                    sx = 15 - x if rv & 0x4000 else x
                    sy = 15 - y if rv & 0x8000 else y
                    v = px[sy * 16 + sx]
                    if v:
                        pix[(ax - 8 + x, ay - 8 + y)] = v
        return pix
    def pcg_pixels(self, fname, idx):
        px = pcg16(self.file(fname), idx)
        return {(x - 8, y - 8): px[y * 16 + x] for y in range(16) for x in range(16) if px[y * 16 + x]}

def split_sprite(pix, group, x68pal, sprpal, tiles):
    """Pixel dict -> list of MD sprite pieces (dx, dy, w, h, tile, pal)."""
    if not pix:
        return []
    xs = [k[0] for k in pix]; ys = [k[1] for k in pix]
    x0, y0 = min(xs), min(ys)
    tw = (max(xs) - x0) // 8 + 1; th = (max(ys) - y0) // 8 + 1
    def tile_at(tx, ty):
        out = [0] * 64; used = False
        for y in range(8):
            for x in range(8):
                v = pix.get((x0 + tx * 8 + x, y0 + ty * 8 + y))
                if v:
                    out[y * 8 + x] = group.index(x68pal[v]); used = True
        return out if used else None
    grid = {(tx, ty): tile_at(tx, ty) for tx in range(tw) for ty in range(th)}
    pieces = []
    for by in range(0, th, 4):
        for bx in range(0, tw, 4):
            cells = [(tx, ty) for tx in range(bx, min(bx + 4, tw)) for ty in range(by, min(by + 4, th)) if grid[(tx, ty)]]
            if not cells:
                continue
            ax0 = min(c[0] for c in cells); ax1 = max(c[0] for c in cells)
            ay0 = min(c[1] for c in cells); ay1 = max(c[1] for c in cells)
            w = ax1 - ax0 + 1; h = ay1 - ay0 + 1
            first = None
            # MD sprite tiles are column-major and must be consecutive
            block = []
            for tx in range(ax0, ax1 + 1):
                for ty in range(ay0, ay1 + 1):
                    block.append(tile_bytes(grid[(tx, ty)] or [0] * 64))
            key = b''.join(block)
            first = tiles.add_block(key, len(block))
            pieces.append((x0 + ax0 * 8, y0 + ay0 * 8, w, h, first, sprpal))
    return pieces

class SpriteTiles:
    def __init__(self, base):
        self.base = base; self.data = bytearray(); self.blocks = {}
    def add_block(self, key, n):
        if key not in self.blocks:
            self.blocks[key] = self.base + len(self.data) // 32
            self.data += key
        return self.blocks[key]


FONT_CHARS = ' 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-:!.'
FONT_ROWS = {
 ' ': '', '0': '0e11131519110e', '1': '040c040404040e', '2': '0e11010e10101f', '3': '1e01010e01011e',
 '4': '02060a121f0202', '5': '1f101e0101111e', '6': '0e10101e11110e', '7': '1f010204080808',
 '8': '0e11110e11110e', '9': '0e11110f01010e', 'A': '0e11111f111111', 'B': '1e11111e11111e',
 'C': '0e11101010110e', 'D': '1e11111111111e', 'E': '1f10101e10101f', 'F': '1f10101e101010',
 'G': '0f10101311110f', 'H': '1111111f111111', 'I': '0e04040404040e', 'J': '0701010101110e',
 'K': '11121418141211', 'L': '1010101010101f', 'M': '111b1515111111', 'N': '11191513111111',
 'O': '0e11111111110e', 'P': '1e11111e101010', 'Q': '0e111111150e01', 'R': '1e11111e141211',
 'S': '0f10100e01011e', 'T': '1f040404040404', 'U': '1111111111110e', 'V': '11111111110a04',
 'W': '11111115151b11', 'X': '11110a040a1111', 'Y': '11110a04040404', 'Z': '1f01020408101f',
 '-': '0000001f000000', ':': '00040000000400', '!': '04040404040004', '.': '00000000000c0c',
}

def font_glyphs():
    """8x8 glyph pixel lists (1 = ink) for FONT_CHARS, drawn with a 1px shadow-free 5x7 face."""
    out = []
    for ch in FONT_CHARS:
        rows = bytes.fromhex(FONT_ROWS[ch]) if FONT_ROWS[ch] else b''
        px = [0] * 64
        for y, b in enumerate(rows):
            for x in range(5):
                if b & (0x10 >> x):
                    px[y * 8 + x + 1] = 1
        out.append(px)
    return out

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

    src = SpriteSrc(xsp_dir)
    # sprite frame list: (symbol, kind, args, x68 palette number, md palette group)
    g_a, g_b = PalGroup('sprA'), PalGroup('sprB')
    frames = []
    for i in range(6):
        frames.append(('PLAYER', ('ref', 'player', i), 9, g_a))
    for i in range(16):
        frames.append(('ZAKO', ('ref', 'zako', i), 7, g_b))
    frames.append(('SHIP', ('ref', 'obj200', 0), 9, g_a))
    for i in range(32):
        frames.append(('BIGBUL', ('ref', 'bullet32', i), 1, g_b))
    for i in range(0, 42, 3):
        frames.append(('EXPL', ('ref', 'expl4', 168 + i), 2, g_a))
    for i in range(6):
        frames.append(('EXPLS', ('ref', 'expl3', i), 2, g_a))
    for i in range(8):
        frames.append(('BUL', ('pcg', 'sz2_main.sp', 0x20 + i), 12, g_b))
    for i in range(2):
        frames.append(('BULS', ('pcg', 'sz2_main.sp', 0x10 + i), 12, g_b))
    frames.append(('SHOTC', ('pcg', 'rot32.sp', 0x48), 5, g_a))
    for i in range(2):
        frames.append(('SHOTS', ('pcg', 'rot32.sp', 0x48 + i), 2, g_a))
    for i in range(2):
        frames.append(('MUZZLE', ('pcg', 'sz2_main.sp', 0x12 + i), 2, g_a))

    fpix = []
    for sym, (kind, f, i), p, g in frames:
        pix = src.ref_pixels(f, i) if kind == 'ref' else src.pcg_pixels(f, i)
        fpix.append(pix)
        for v in pix.values():
            g.add(sppal[p * 16 + v])
    g_a.build(); g_b.build()
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

    # ---- sprites
    stiles = SpriteTiles(base=nbg)
    frame_pieces = []
    for (sym, spec, p, g), pix in zip(frames, fpix):
        frame_pieces.append(split_sprite(pix, g, sppal[p * 16:p * 16 + 16], groups.index(g), stiles))
    nspr = len(stiles.data) // 32
    print(f'BG tiles {nbg}, sprite tiles {nspr}, total {nbg + nspr}')
    assert nbg + nspr <= 1536, 'VRAM tile budget exceeded'

    # ---- 8x8 font for score / messages (own glyphs; SZ_TEXT.pcg is not used)
    white = g_a.index((255, 255, 255))
    font_tiles = b''.join(tile_bytes([white if c else 0 for c in g]) for g in font_glyphs())
    font_base = 0xF000 // 32           # VRAM 0xF000-0xFFFF is free in our layout
    assert len(font_tiles) // 32 <= 128

    # ---- tables from SZ2.X
    trig = struct.unpack_from('>16384h', img, 0x3e3bc)         # [32 speeds][256 angles][dx,dy]
    atan = img[0x2fb68:0x2fb68 + 257]                          # index (ratio+128), ratio -128..128
    rnd = struct.unpack_from('>1024H', img, 0x345e2)
    shotdir = struct.unpack_from('>512h', img, 0x3dbbc)        # [angle][ref offset, info xor]
    power = struct.unpack_from('>30h', img, 0x489a6)            # 5 levels x 6 words
    zako_angle = img[0x489ea:0x489ea + 10]                      # joystick dir -> angle
    classes = {t: struct.unpack_from('>IIIH', img, 0x46460 + t * 16) for t in (0x190, 0x1f0)}

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
    out.append(arr('u32', 'spr_tiles', list(struct.unpack(f'>{len(stiles.data)//4}I', bytes(stiles.data))), 8, '0x{:08x}'))
    out.append(arr('u32', 'font_tiles', list(struct.unpack(f'>{len(font_tiles)//4}I', font_tiles)), 8, '0x{:08x}'))
    # chips: index by original chip number (256 entries x 16 words), unused -> 0
    cw = []
    for ch in range(256):
        cw += chip_near.get(ch, [0] * 16)
    out.append(arr('u16', 'chip_near', cw, fmt='0x{:04x}'))
    cw = []
    for ch in range(256):
        cw += chip_far.get(ch, [0] * 16)
    out.append(arr('u16', 'chip_far', cw, fmt='0x{:04x}'))
    out.append(arr('u8', 'map_bg0', list(m0), 8))
    out.append(arr('u8', 'map_bg1', list(m1), 8))
    # sprite frames
    pieces = []; fidx = []; syms = collections.OrderedDict()
    for (sym, *_), pc in zip(frames, frame_pieces):
        syms.setdefault(sym, len(fidx))
        fidx.append(len(pieces))
        for dx, dy, w, h, t, pal in pc:
            pieces.append((dx, dy, ((w - 1) << 2) | (h - 1), (pal << 13) | t))
    fidx.append(len(pieces))
    out.append(arr('u16', 'frame_start', fidx))
    out.append('const SprPiece frame_pieces[%d] = {' % len(pieces))
    for dx, dy, sz, attr in pieces:
        out.append(f'  {{{dy}, {sz}, 0x{attr:04x}, {dx}}},')
    out.append('};')
    out.append(arr('s16', 'trig_tab', list(trig), 16))
    out.append(arr('u8', 'atan_tab', list(atan), 16))
    out.append(arr('u16', 'rand_tab', list(rnd), 16, '0x{:04x}'))
    out.append(arr('s16', 'shotdir_tab', list(shotdir), 16))
    out.append(arr('s16', 'power_tab', list(power), 6))
    out.append(arr('u8', 'dir_angle_tab', list(zako_angle), 10))
    open(os.path.join(out_dir, 'assets.c'), 'w').write('\n'.join(out) + '\n')

    h = ['/* generated by tools/convert_assets.py - do not edit */',
         '#ifndef ASSETS_H', '#define ASSETS_H', '#include "types.h"', '',
         'typedef struct { s8 dy; u8 size; u16 attr; s16 dx; } SprPiece;', '',
         f'#define BG_TILE_COUNT {nbg}', f'#define SPR_TILE_BASE {nbg}', f'#define SPR_TILE_COUNT {nspr}',
         f'#define FONT_TILE_BASE {font_base}', f'#define FONT_TILE_COUNT {len(font_tiles)//32}',
         f'#define FONT_CHARS "{FONT_CHARS}"', '']
    for sym, start in syms.items():
        h.append(f'#define FR_{sym} {start}')
    h.append(f'#define FRAME_COUNT {len(frames)}')
    for t, (upd, ini, score, hp) in classes.items():
        h.append(f'#define SCORE_{t:03X} {score}')
        h.append(f'#define HP_{t:03X} {hp}')
    h += ['', 'extern const u16 pal_data[64];',
          f'extern const u32 bg_tiles[{len(alltiles)//4}];',
          f'extern const u32 spr_tiles[{len(stiles.data)//4}];',
          f'extern const u32 font_tiles[{len(font_tiles)//4}];',
          'extern const u16 chip_near[4096];', 'extern const u16 chip_far[4096];',
          'extern const u8 map_bg0[8192];', 'extern const u8 map_bg1[8192];',
          f'extern const u16 frame_start[{len(fidx)}];',
          f'extern const SprPiece frame_pieces[{len(pieces)}];',
          'extern const s16 trig_tab[16384];', 'extern const u8 atan_tab[257];',
          'extern const u16 rand_tab[1024];', 'extern const s16 shotdir_tab[512];',
          'extern const s16 power_tab[30];', 'extern const u8 dir_angle_tab[10];',
          '#endif']
    open(os.path.join(out_dir, 'assets.h'), 'w').write('\n'.join(h) + '\n')
    for g in groups:
        print(g.name, len(g.count), 'colors ->', len(g.colors))

if __name__ == '__main__':
    main(*sys.argv[1:4])
