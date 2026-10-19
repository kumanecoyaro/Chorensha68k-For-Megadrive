#!/usr/bin/env python3
"""Convert every XSP sprite of Chorensha68k for the Mega Drive sprite engine (src/xsp.c).

usage: convert_sprites.py XSP_DAT_DIR OUT_DIR

The original loads all PCG files into one PCG pool and all .ref/.frm
composite definitions into one table (fn_04d02); xsp_set() takes a global
PCG number, xobj_set() a global ref number.  This script rebuilds both
numberings and stores, pre-expanded in ROM:

  * blocks: tile data for one Mega Drive hardware sprite (1x1 .. 4x4 tiles,
    column-major, 4bpp with the X68000 colour index 0-15 kept as is; the
    palette is applied when the block is copied to VRAM).  Every PCG is a
    2x2 block; every ref is composited (its frm entries with their own flips)
    and cut into blocks of up to 32x32 pixels, so a 128x80 enemy needs about
    a dozen hardware sprites instead of 40.  Identical blocks are stored once.
  * ref pieces: (dx, dy, size, block, palette xor) relative to the object.
  * palettes: the 16 X68000 sprite palettes are reduced to two Mega Drive
    palettes (2 and 3); each X68000 palette gets a slot and a byte LUT
    (2 pixels per byte) that remaps its colours into that slot.
"""
import os, sys, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x68gfx import load_pal, pcg16

PCG_FILES = ['sz2_main.sp', 'rot32.sp', 'rot16.sp', 'debris.sp', 'mob.sp']
XSP = ['title', 'bullet32', 'bullet_l', 'sonic1', 'sonic2', 'expl2', 'expl3', 'expl4', 'spark',
       'debris2', 'wave', 'flare', 'score', 'player', 'player2', 'zako', 'obj204_d', 'parts32',
       'obj1a0', 'obj200', 'obj201', 'obj202', 'obj203', 'obj204', 'obj205', 'obj207', 'obj208',
       'obj20A', 'obj20E', 'obj20F', 'obj210', 'obj212', 'obj213', 'obj215', 'obj216', 'obj21a',
       'obj21b', 'obj21d', 'obj21f', 'obj220', 'obj232', 'obj242']
# ref files in load order; base = PCG number added to the frm entries
# (None = the PCG file loaded just before, else an explicit number)
REF_EXTRA = {'sonic2': [('ele_arc', 0x50)], 'spark': [('debris1', 0xC4)]}

TEXT_XPAL = 2          # X68000 sprite palette used as the text palette
MERGE_MIN = 4          # refs with more frm entries are composited into larger blocks
# effects made of many scattered / mirrored PCGs: compositing them would cost
# megabytes of ROM, so they keep one hardware sprite per PCG
NO_MERGE = {'debris1', 'debris2', 'expl4', 'sonic1', 'sonic2', 'ele_arc'}

def md_rgb(c):
    return tuple(min(7, (v * 7 + 127) // 255) for v in c)

def md_word(c):
    r, g, b = c
    return (b << 9) | (g << 5) | (r << 1)

def d2(a, b):
    return 3 * (a[0] - b[0]) ** 2 + 4 * (a[1] - b[1]) ** 2 + 2 * (a[2] - b[2]) ** 2

def kmeans(items, n):
    """items: {(r,g,b): weight} in 3-bit space -> <= n colours."""
    cols = sorted(items.items(), key=lambda t: -t[1])
    if len(cols) <= n:
        return [c for c, _ in cols]
    cent = [c for c, _ in cols[:n]]
    for _ in range(25):
        acc = [[0, 0, 0, 0] for _ in cent]
        for c, w in cols:
            k = min(range(n), key=lambda i: d2(c, cent[i]))
            a = acc[k]; a[0] += c[0] * w; a[1] += c[1] * w; a[2] += c[2] * w; a[3] += w
        new = [tuple(int(round(a[i] / a[3])) for i in range(3)) if a[3] else o for a, o in zip(acc, cent)]
        if new == cent:
            break
        cent = new
    return cent

class Blocks:
    def __init__(self):
        self.data = bytearray(); self.index = {}; self.table = []    # (tile offset, w, h)
    def add(self, w, h, tiles):
        key = (w, h, tiles)
        if key not in self.index:
            self.index[key] = len(self.table)
            self.table.append((len(self.data) // 32, w, h))
            self.data += tiles
        return self.index[key]

def tile_bytes(px):
    return bytes((px[i] << 4) | px[i + 1] for i in range(0, 64, 2))

def main(xsp, out):
    rd = lambda f: open(os.path.join(xsp, f), 'rb').read()
    # ---- global PCG pool
    pcg = bytearray(); bases = {}
    for f in PCG_FILES:
        bases[f] = len(pcg) // 128; pcg += rd(f)
    for n in XSP:
        bases[n] = len(pcg) // 128; pcg += rd(n + '.xsp')
    npcg = len(pcg) // 128
    blocks = Blocks()
    pcg_block = [blocks.add(2, 2, bytes(pcg[i * 128:(i + 1) * 128])) for i in range(npcg)]

    # ---- refs
    reflist = []
    for n in XSP:
        reflist.append((n, bases[n]))
        reflist += REF_EXTRA.get(n, [])
    idx_freq = collections.Counter()
    pieces = []; ref_start = []
    per_file = []
    for name, base in reflist:
        before = len(blocks.data)
        per_file.append((name, lambda b=before: len(blocks.data) - b))
        ref = rd(name + '.ref'); frm = rd(name + '.frm')
        for r in range(len(ref) // 8):
            cnt, off = struct.unpack_from('>hI', ref, r * 8)
            ref_start.append(len(pieces))
            groups = collections.OrderedDict()
            ax = ay = 0
            if cnt <= MERGE_MIN or name in NO_MERGE:
                # few PCGs: one hardware sprite each, flips done by the VDP
                for j in range(cnt):
                    vx, vy, pt, info = struct.unpack_from('>hhhH', frm, off + j * 8)
                    ax += vx; ay += vy
                    for v in pcg[(pt + base) * 128:(pt + base + 1) * 128]:
                        idx_freq[v >> 4] += 1; idx_freq[v & 15] += 1
                    pieces.append((ax - 16, ay - 16, 5 | ((info >> 10) & 0x30), pcg_block[pt + base],
                                   (info >> 8) & 15))
                continue
            for j in range(cnt):
                vx, vy, pt, info = struct.unpack_from('>hhhH', frm, off + j * 8)
                ax += vx; ay += vy
                g = groups.setdefault(info & 0x0F00, {})
                px = pcg16(pcg, pt + base)
                for y in range(16):
                    for x in range(16):
                        sx = 15 - x if info & 0x4000 else x
                        sy = 15 - y if info & 0x8000 else y
                        v = px[sy * 16 + sx]
                        if v:
                            # relative to the object centre (sprite coordinate x>>6 is centre + 8)
                            g[(ax - 16 + x, ay - 16 + y)] = v
            for palx, pix in groups.items():
                if not pix:
                    continue
                for v in pix.values():
                    idx_freq[v] += 1
                xs = [p[0] for p in pix]; ys = [p[1] for p in pix]
                x0, y0 = min(xs), min(ys)
                tw = (max(xs) - x0) // 8 + 1; th = (max(ys) - y0) // 8 + 1
                def tile(tx, ty):
                    t = [pix.get((x0 + tx * 8 + x, y0 + ty * 8 + y), 0) for y in range(8) for x in range(8)]
                    return t if any(t) else None
                grid = {(tx, ty): tile(tx, ty) for tx in range(tw) for ty in range(th)}
                for by in range(0, th, 4):
                    for bx in range(0, tw, 4):
                        cells = [c for c in grid if bx <= c[0] < bx + 4 and by <= c[1] < by + 4 and grid[c]]
                        if not cells:
                            continue
                        cx0 = min(c[0] for c in cells); cx1 = max(c[0] for c in cells)
                        cy0 = min(c[1] for c in cells); cy1 = max(c[1] for c in cells)
                        w = cx1 - cx0 + 1; h = cy1 - cy0 + 1
                        data = b''.join(tile_bytes(grid[(tx, ty)] or [0] * 64)
                                        for tx in range(cx0, cx1 + 1) for ty in range(cy0, cy1 + 1))
                        b = blocks.add(w, h, data)
                        pieces.append((x0 + cx0 * 8, y0 + cy0 * 8, ((w - 1) << 2) | (h - 1), b, palx >> 8))
    ref_start.append(len(pieces))
    nref = len(ref_start) - 1
    if os.environ.get('SPR_STATS'):
        sizes = []
        for i, (n, f) in enumerate(per_file):
            sizes.append((n, f()))
        prev = 0
        for n, tot in sizes:
            pass

    # ---- palettes: 16 X68000 palettes -> MD palettes 2 / 3
    sp = load_pal(os.path.join(xsp, 'SZ_PALET.pal'))
    xp = [[md_rgb(sp[p * 16 + i]) for i in range(16)] for p in range(16)]
    slot = [p & 1 for p in range(16)]
    weight = [1.0] * 16
    weight[15] = 0.05                                   # hit flash (all white)
    for it in range(6):
        cents = []
        for s in range(2):
            items = collections.Counter()
            for p in range(16):
                if slot[p] == s:
                    for i in range(1, 16):
                        items[xp[p][i]] += weight[p] * (idx_freq[i] + 1)
            cents.append(kmeans(items, 15))
        new = []
        for p in range(16):
            err = [sum((idx_freq[i] + 1) * min(d2(xp[p][i], c) for c in cents[s]) for i in range(1, 16))
                   for s in range(2)]
            new.append(0 if err[0] <= err[1] else 1)
        if new == slot:
            break
        slot = new
    cram = []
    for s in range(2):
        c = cents[s] + [(0, 0, 0)] * (15 - len(cents[s]))
        cram.append([0] + [md_word(v) for v in c])
    lut = []
    for p in range(16):
        m = [0] + [1 + min(range(len(cents[slot[p]])), key=lambda k: d2(xp[p][i], cents[slot[p]][k]))
                   for i in range(1, 16)]
        lut.append([(m[b >> 4] << 4) | m[b & 15] for b in range(256)])

    # ---- text font (SZ_TEXT.pcg: 128 chars, 4 bit planes).  The game sets the
    # text palette to sprite palette 2 (fn_051b8: _TPALET2 from the palette
    # file's colours 33-47), so the font is pre-mapped to its Mega Drive palette
    fd = rd('SZ_TEXT.pcg')
    m0 = [lut[TEXT_XPAL][v] & 15 for v in range(16)]
    font = bytearray()
    for ch in range(128):
        o = (ch >> 2) * 128 + (ch & 3) * 16
        if (ch & 3) >= 2:
            o -= 31
        px = []
        for r in range(8):
            for x in range(8):
                v = sum(((fd[o + 0x20 * p + 2 * r] >> (7 - x)) & 1) << p for p in range(4))
                px.append(m0[v])
        font += tile_bytes(px)

    # ---- write
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'spr_tiles.bin'), 'wb').write(blocks.data)
    L = ['/* generated by tools/convert_sprites.py - do not edit */', '#include "spr.h"', '']
    def arr(t, n, vals, per=16):
        L.append(f'const {t} {n}[{len(vals)}] = {{')
        for i in range(0, len(vals), per):
            L.append('  ' + ','.join(str(v) for v in vals[i:i + per]) + ',')
        L.append('};')
    arr('u16', 'pcg_block', pcg_block)
    arr('u16', 'ref_start', ref_start)
    L.append(f'const SprPiece ref_pieces[{len(pieces)}] = {{')
    for dx, dy, sz, b, px in pieces:
        L.append(f'  {{{dx},{dy},{sz},{px},{b}}},')
    L.append('};')
    L.append(f'const SprBlock spr_blocks[{len(blocks.table)}] = {{')
    for off, w, h in blocks.table:
        L.append(f'  {{{off},{w * h}}},')
    L.append('};')
    arr('u16', 'spr_cram', cram[0] + cram[1])
    L.append('const u32 text_font[1024] __attribute__((aligned(2))) = {')
    fw = struct.unpack(f'>{len(font) // 4}I', bytes(font))
    for i in range(0, len(fw), 8):
        L.append('  ' + ','.join(f'0x{v:08x}' for v in fw[i:i + 8]) + ',')
    L.append('};')
    arr('u8', 'pal_slot', [2 + s for s in slot])
    arr('u8', 'pal_lut', sum(lut, []), 32)
    open(os.path.join(out, 'spr.c'), 'w').write('\n'.join(L) + '\n')
    H = ['/* generated by tools/convert_sprites.py - do not edit */', '#ifndef SPR_H', '#define SPR_H',
         '#include "types.h"',
         '/* dx, dy: top-left relative to the sprite position (x >> 6, y >> 6);',
         '   size: MD sprite size bits (w-1)<<2|(h-1), bit 4 = H flip, bit 5 = V flip */',
         'typedef struct { s16 dx, dy; u8 size, palx; u16 block; } SprPiece;',
         'typedef struct { u32 tile; u16 ntiles; } SprBlock;  /* tile = offset in spr_tiles (32-byte units) */',
         f'#define NPCG {npcg}', f'#define NREF {nref}', f'#define NBLOCK {len(blocks.table)}',
         f'extern const u16 pcg_block[{npcg}];', f'extern const u16 ref_start[{nref + 1}];',
         f'extern const SprPiece ref_pieces[{len(pieces)}];', f'extern const SprBlock spr_blocks[{len(blocks.table)}];',
         'extern const u16 spr_cram[32];', 'extern const u8 pal_slot[16];', 'extern const u8 pal_lut[16 * 256];',
         'extern const u8 spr_tiles[];', 'extern const u32 text_font[1024];',
         f'#define TEXT_PAL {slot[TEXT_XPAL] + 2}',
         '#endif', '']
    open(os.path.join(out, 'spr.h'), 'w').write('\n'.join(H))
    open(os.path.join(out, 'spr_def.inc'), 'w').write(f'| generated by tools/convert_sprites.py\n'
                                                      f'    .set NPCG, {npcg}\n    .set NREF, {nref}\n    .set TEXT_PAL, {slot[TEXT_XPAL] + 2}\n')
    print(f'sprites: {npcg} PCGs, {nref} refs, {len(pieces)} pieces, {len(blocks.table)} blocks, '
          f'{len(blocks.data) // 1024} KB tiles; palette slots {"".join(str(s) for s in slot)}')

if __name__ == '__main__':
    main(*sys.argv[1:3])
