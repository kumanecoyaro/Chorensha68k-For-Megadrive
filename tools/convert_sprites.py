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
import os, math, sys, struct, collections
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
# large enemies drawn on plane A (src/plane.c): groups of images (one group's
# tiles are in VRAM at a time); image = refs (file, index, x offset, object
# palette[, y offset, H flip]) composited in drawing order (later = front)
# the stage 2 mid boss's arm poses (left side: upper arm x, y, lower arm x, y)
O200_POSE = [(-48, -32, -56, 8), (-56, -36, -64, 12)]
PLANE_GROUPS = [
    [   # obj202 crab: halves 0..4 px apart, as two images (0 and 4 px)
        [('obj202', 0, 0, 6), ('obj202', 1, 0, 6), ('obj202', 2, 0, 5)],
        [('obj202', 0, -4, 6), ('obj202', 1, 4, 6), ('obj202', 2, 0, 5)],
    ],
    [   # obj204 stage 1 boss
        [('obj204', 0, 0, 7)],
    ],
    [   # obj21f (two of them after a quickly destroyed crab): $B79, $B7A after it fires
        [('obj21f', 0, 0, 7)],
        [('obj21f', 1, 0, 7)],
    ],
    [   # stage 2 mid boss obj200 (type $200, two of them, palettes 8 / 7): body $B18, arms
        # $B19 / $B1A either side (the right ones mirrored), closed: 48 / 56 px out (the
        # usual pose); the body alone (the arms moving / open stay sprites)
        # (both palettes in one group: the same tiles, and the two show together,
        # src/plane.c PAIR_IMG)
        [('obj200', 0, 0, 8), ('obj200', 1, -48, 8, -32), ('obj200', 1, 48, 8, -32, 1),
         ('obj200', 2, -56, 8, 8), ('obj200', 2, 56, 8, 8, 1)],
        [('obj200', 0, 0, 8)],
        [('obj200', 0, 0, 7), ('obj200', 1, -48, 7, -32), ('obj200', 1, 48, 7, -32, 1),
         ('obj200', 2, -56, 7, 8), ('obj200', 2, 56, 7, 8, 1)],
        [('obj200', 0, 0, 7)],
    ],
    [   # stage 2 boss obj205: core $B31, with its pods $B2B..$B30 (pod animation)
        [('obj205', 6, 0, 7)],
        [('obj205', 0, 0, 7)], [('obj205', 1, 0, 7)], [('obj205', 2, 0, 7)],
        [('obj205', 3, 0, 7)], [('obj205', 4, 0, 7)], [('obj205', 5, 0, 7)],
    ],
    [   # stage 3 boss obj207: $B34..$B3B (its frames: 273 tiles together)
        [('obj207', i, 0, 7)] for i in range(8)
    ],
    [   # its second form $B3C, $B3D
        [('obj207', 8, 0, 7)], [('obj207', 9, 0, 7)],
    ],
    [   # stage 3 large enemy type $1F0 (obj200 body $B18 alone, palette 9)
        [('obj200', 0, 0, 9)],
    ],
    [   # stage 3 floor enemy type $1A1 (obj1a0): pods $B17 either side (the right one
        # mirrored) 28 px out, 12 px up, centre $B16, palette 6: the pods fully out
        [('obj1a0', 2, -28, 6, -12), ('obj1a0', 2, 28, 6, -12, 1), ('obj1a0', 1, 0, 6)],
    ],
    [   # stage 4 battleship type $20A (obj20A, palette 6): centre $B43, wings $B44 128 px
        # either side, 24 px up (the right one mirrored), drawn in front of the centre; the
        # whole ship from the centre's position, one image per set of parts there
        # (src/plane.c IMG_SHIP + mask - 1: bit 0 left wing, 1 right wing, 2 centre)
        [p for b, p in ((4, ('obj20A', 0, 0, 6)), (1, ('obj20A', 1, -128, 6, -24)),
                        (2, ('obj20A', 1, 128, 6, -24, 1))) if m & b]
        for m in range(1, 8)
    ],
    [   # stage 4 large ship type $20E (obj20E, 96 x 192 px, palette 9): its 8 frames $B46..$B4D
        [('obj20E', i, 0, 9)] for i in range(8)
    ],
    [   # stage 5 battleship obj21a (palette 6, 128 x 256 px): $B75, damaged $B76
        [('obj21a', 0, 0, 6)], [('obj21a', 1, 0, 6)],
    ],
    [   # stage 5 mid boss obj213 (palette 3, 160 x 80 px): $B5B
        [('obj213', 0, 0, 3)],
    ],
    [   # stage 5 boss obj216 (palette 8): the body $B5E alone, and with each of the parts
        # drawn over it ($B5F..$B67, at the body's position); the arms ($B68..) stay sprites
        [('obj216', 0, 0, 8)]] + [[('obj216', 0, 0, 8), ('obj216', k, 0, 8)] for k in range(1, 10)
    ],
    [   # stage 6 boss obj210 (palette 3, 384 x 240 px: more tiles than the plane A enemy
        # VRAM, src/plane.c LEND_SLOTS): $B51..$B54, each drawn twice at one position (the
        # second mirrored: the two halves); $B52 with $B58 16 px either side, $B53 with $B57
        # 16 px above it (mirrored too; while they move they stay sprites); src/plane.c
        # IMG_O210 + 0 .. 6: $B51, $B52, $B52 + $B58, $B53, $B54, $B53 + $B57, $B55
        [('obj210', 0, 0, 3), ('obj210', 0, 0, 3, 0, 1)],
        [('obj210', 1, 0, 3), ('obj210', 1, 0, 3, 0, 1)],
        [('obj210', 1, 0, 3), ('obj210', 1, 0, 3, 0, 1), ('obj210', 7, -16, 3), ('obj210', 7, 16, 3, 0, 1)],
    ],
    [
        [('obj210', 2, 0, 3), ('obj210', 2, 0, 3, 0, 1)],
        [('obj210', 3, 0, 3), ('obj210', 3, 0, 3, 0, 1)],
    ],
    [
        [('obj210', 2, 0, 3), ('obj210', 2, 0, 3, 0, 1), ('obj210', 6, 0, 3, -16), ('obj210', 6, 0, 3, -16, 1)],
    ],
    [   # its last form but one $B55 (mirrored too; the last one $B56: a group of its own below)
        [('obj210', 4, 0, 3), ('obj210', 4, 0, 3, 0, 1)],
    ],
    [   # obj21f in stage 6 (palette 9): $B79, $B7A after it fires
        [('obj21f', 0, 0, 9)],
        [('obj21f', 1, 0, 9)],
    ],
    [   # the stage 2 mid boss obj200 whole (palettes 8, 7): the body $B18 with its arms in
        # closed or open each side (src/plane.c o200_pose: upper arm $B19, lower arm $B1A,
        # the right ones mirrored; the cells on an 8 px grid from the body); IMG_O200P +
        # palette (8: 0, 7: 4) + left pose * 2 + right pose
        [('obj200', 0, 0, p),
         ('obj200', 1, O200_POSE[li][0], p, O200_POSE[li][1]),
         ('obj200', 1, -O200_POSE[ri][0], p, O200_POSE[ri][1], 1),
         ('obj200', 2, O200_POSE[li][2], p, O200_POSE[li][3]),
         ('obj200', 2, -O200_POSE[ri][2], p, O200_POSE[ri][3], 1)]
        for p in (8, 7) for li in range(2) for ri in range(2)
    ] + [[('obj200', 0, 0, 8)], [('obj200', 0, 0, 7)]],     # (+ 8 / 9: the body alone)
    [   # stage 6 large enemy obj20F (palette 8, two at a time): the centre $B50, $B4F 23 px
        # either side, the pods $B4E 52 px either side (the right ones mirrored)
        [('obj20F', 2, 0, 8), ('obj20F', 1, -23, 8, 4), ('obj20F', 1, 22, 8, 4, 1),
         ('obj20F', 0, -52, 8, -13), ('obj20F', 0, 51, 8, -13, 1)],
    ],
    [   # stage 3 formations of type $1A1 (obj1a0, palette 6): folded $B15, the pods out
        # (as IMG_O1A1): src/plane.c formation mode (IMG_FM: several of them on plane A)
        [('obj1a0', 0, 0, 6)],
        [('obj1a0', 2, -28, 6, -12), ('obj1a0', 2, 28, 6, -12, 1), ('obj1a0', 1, 0, 6)],
    ] + [   # (+ 2 ..: the pods opening, 16 + s px out and s up, s = 2, 4 .. 10)
        [('obj1a0', 2, -16 - sp, 6, -sp), ('obj1a0', 2, 16 + sp, 6, -sp, 1), ('obj1a0', 1, 0, 6)]
        for sp in range(2, 12, 2)
    ],
    [   # stage 6 large enemy obj212 (palette 7): centre $B59, the wings $B5A d px either
        # side and 28 - d up (the right one mirrored), closed (d = 28) and open (d = 36);
        # src/plane.c IMG_O212 (several at once on plane A, formation mode)
        [('obj212', 1, -28, 7), ('obj212', 1, 28, 7, 0, 1), ('obj212', 0, 0, 7)],
        [('obj212', 1, -36, 7, -8), ('obj212', 1, 36, 7, -8, 1), ('obj212', 0, 0, 7)],
    ],
    [   # the stage 6 boss's last form $B56 (obj210, palette 3): src/plane.c IMG_O210F
        [('obj210', 5, 0, 3)],
    ],
]
BOMB_REFS = list(range(0xB6, 0xC0)) + list(range(0xAA9, 0xAAF))   # expl2 / expl3 / flare (src/plane.c is_effect)
BOMB_PALS = (1, 3, 5, 12)  # X68000 palettes the bomb draws them with (info $x13F / $xC3F ...)
MERGE_MIN = 1          # refs with more frm entries are composited into blocks of up to 32x32
                       # (a 32x32 enemy made of four 16x16 PCGs becomes one hardware sprite)
# effects made of many scattered / mirrored PCGs: compositing them would cost
# megabytes of ROM, so they keep one hardware sprite per PCG
VLASER_PCGS = (0x50, 0x59)   # the vertical laser segments, thick / thin: refs VLASER_REF, + 1
# laser segments the bosses fire at an angle (src/xsp_fast.s h_xsp_set_boss): two of
# them next to each other as one ref, for every step (second minus first, unflipped)
# 14.5 to 17.5 px long within 11 degrees of the segment's direction (its pixels'
# principal axis), either way round: the steps seen in the stage 2 / 3 bosses lie
# within 9.3 degrees, 15 to 17.1 px (PAIR_REF + pair_lut[pcg - $50][dy + 16][dx + 16] - 1;
# the vertical $50 / $59 are merged by h_xsp_set_main)
PAIR_PCGS = [p for p in range(0x51, 0x60) if p != 0x59]
PAIR_DIST = (14.5, 17.5)
PAIR_ANGLE = 11.0
# the stage 3 boss's needle streams ($86, palette 1, src/xsp_fast.s h_xsp_set_main):
# two lanes 8 px apart, the second ~10 px lower (table row 16)
NEEDLE_PCG = 0x86
NEEDLE_STEPS = [(dx, dy) for dx in (-8, 8) for dy in (9, 10, 11, 12)]
# the stage 5 battleship's turrets (palette 6): the twin barrels, the same PCG
# twice 11 to 16 px apart, as one 32 x 32 sprite (table rows 17.., the steps
# seen in play: tools/turret_pairs.txt)
TURRET_LO, TURRET_HI = 0x6B, 0x7C
TURRET_ROW = 17
# the call sites of the second barrels (tools/relocate.py TURRET_PAIR2): first / second
# barrel's angle offset, y table, x table index offset, PCG offset, object PCG base
# ($4(a3) of the turret types $62 / $50), the angle in a1 is the second barrel's
TURRET_SITES = [(0x48, 0xB8, 0x403be, 0x800, 9, 0x62, False),
                (0x10, -0x10, 0x423be, 0x1000, 18, 0x62, True),
                (0x48, 0xB8, 0x403be, 0x800, 27, 0x50, False),
                (0x08, -0x08, 0x45bbe, 0x1e00, 36, 0x50, True)]
# the stage 3 boss's thick beams: lanes of refs 1-3 (16 x 32 flames), two of them
# 16 px apart side by side as one 32 x 32 sprite (src/xsp_fast.s h_xobj_set: LBEAM_REF + ref - 1)
LBEAM_REFS = (1, 2, 3)
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

class TileSet:
    def __init__(self):
        self.tiles = []; self.index = {}
    def add(self, data):
        if data not in self.index:
            self.index[data] = len(self.tiles)
            self.tiles.append(data)
        return self.index[data]

def tile_bytes(px):
    return bytes((px[i] << 4) | px[i + 1] for i in range(0, 64, 2))

def add_flipped(ts, px):
    """a plane image tile: one already in the set, flipped (name table flip
    bits 11 / 12: mirrored images take half the tiles), else a new one;
    -> plane map word (flips | tile + 1)"""
    for flips in (0, 0x800, 0x1000, 0x1800):
        q = [px[(7 - y if flips & 0x1000 else y) * 8 + (7 - x if flips & 0x800 else x)]
             for y in range(8) for x in range(8)]
        k = ts.index.get(tile_bytes(q))
        if k is not None:
            return flips | (k + 1)
    return ts.add(tile_bytes(px)) + 1

def main(xsp, out, sz2x=None):
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
    ref_first = {}; ref_src = []
    def emit(groups):
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
    for name, base in reflist:
        ref_first[name] = len(ref_start)
        before = len(blocks.data)
        per_file.append((name, lambda b=before: len(blocks.data) - b))
        ref = rd(name + '.ref'); frm = rd(name + '.frm')
        for r in range(len(ref) // 8):
            cnt, off = struct.unpack_from('>hI', ref, r * 8)
            ref_src.append((frm, off, cnt, base))
            ref_start.append(len(pieces))
            groups = collections.OrderedDict()
            ax = ay = 0
            if cnt <= MERGE_MIN or name in NO_MERGE:
                # a single PCG / scattered effects: one hardware sprite per PCG, flips done by the VDP
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
            emit(groups)
    freq_kept = collections.Counter(idx_freq)   # (the synthetic refs below don't change the palette slots)
    # synthetic refs (src/xsp_fast.s h_xsp_set_main): two vertical laser segments
    # one above the other (the lasers fired straight down) as one 16 x 32 sprite
    for p in VLASER_PCGS:
        ref_start.append(len(pieces))
        px = pcg16(pcg, p)
        g = {}
        for y in range(16):
            for x in range(16):
                if px[y * 16 + x]:
                    g[(x - 16, y - 16)] = g[(x - 16, y)] = px[y * 16 + x]
        emit({0: g})
    vlaser_ref = len(ref_start) - len(VLASER_PCGS)
    pair_ref = len(ref_start)
    pair_lut = [0] * ((TURRET_ROW + TURRET_HI - TURRET_LO + 1) * 33 * 33)
    def axis(p):
        px = pcg16(pcg, p)
        pts = [(i % 16, i // 16) for i, v in enumerate(px) if v]
        mx = sum(a for a, _ in pts) / len(pts); my = sum(b for _, b in pts) / len(pts)
        sxx = sum((a - mx) ** 2 for a, _ in pts); syy = sum((b - my) ** 2 for _, b in pts)
        sxy = sum((a - mx) * (b - my) for a, b in pts)
        return math.degrees(0.5 * math.atan2(2 * sxy, sxx - syy))
    steps = []
    for p in PAIR_PCGS:
        th = axis(p)
        offs = [(dx, dy) for dx in range(-16, 17) for dy in range(-16, 17)
                if PAIR_DIST[0] <= math.hypot(dx, dy) <= PAIR_DIST[1]
                and abs((math.degrees(math.atan2(dy, dx)) - th + 90) % 180 - 90) <= PAIR_ANGLE]
        steps.append((p, p - 0x50, offs))
    steps.append((NEEDLE_PCG, 16, [(dx * sgn, dy * sgn) for dx, dy in NEEDLE_STEPS for sgn in (1, -1)]))
    # turrets: the steps seen, most frequent first, each taking the steps within 1 px
    # (the second barrel drawn up to 1 px off: far fewer refs, so their tiles stay cached)
    tur = {}
    for ln in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'turret_pairs.txt')):
        if ln.strip() and not ln.startswith('#'):
            p, dx, dy, n = ln.split()
            tur.setdefault(int(p, 16), {})[(int(dx), int(dy))] = int(n)
    tur_alias = []                          # (lut index, canonical step's lut index)
    for p in range(TURRET_LO, TURRET_HI + 1):
        seen = tur.get(p, {}); row = TURRET_ROW + p - TURRET_LO; canon = []
        left = sorted(seen, key=lambda o: -seen[o])
        while left:
            c = left[0]; canon.append(c)
            left = [o for o in left if max(abs(o[0] - c[0]), abs(o[1] - c[1])) > 1]
        steps.append((p, row, canon))
        claimed = set(canon)
        for c in canon:
            for ex in (-1, 0, 1):
                for ey in (-1, 0, 1):
                    o = (c[0] + ex, c[1] + ey)
                    if o not in claimed and abs(o[0]) <= 16 and abs(o[1]) <= 16:
                        claimed.add(o)
                        tur_alias.append((row * 33 * 33 + (o[1] + 16) * 33 + o[0] + 16,
                                          row * 33 * 33 + (c[1] + 16) * 33 + c[0] + 16))
    def emit_steps(turrets):
        for p, row, offs in steps:
            if (row >= TURRET_ROW) != turrets:
                continue
            px = pcg16(pcg, p)
            for dx, dy in offs:
                if abs(dx) > 16 or abs(dy) > 16:
                    continue
                k = row * 33 * 33 + (dy + 16) * 33 + dx + 16
                if pair_lut[k]:
                    continue
                ref_start.append(len(pieces))
                g = {}
                for y in range(16):
                    for x in range(16):
                        if px[y * 16 + x]:
                            g[(x - 16, y - 16)] = g[(x - 16 + dx, y - 16 + dy)] = px[y * 16 + x]
                emit({0: g})
                pair_lut[k] = len(ref_start) - pair_ref
    emit_steps(False)
    lbeam_ref = len(ref_start)
    for r in LBEAM_REFS:
        frm, off, cnt, base = ref_src[r]
        ax = ay = 0; g = {}
        for j in range(cnt):
            vx, vy, pt, info = struct.unpack_from('>hhhH', frm, off + j * 8)
            ax += vx; ay += vy
            assert not info & 0x0F00
            px = pcg16(pcg, pt + base)
            for y in range(16):
                for x in range(16):
                    v = px[(15 - y if info & 0x8000 else y) * 16 + (15 - x if info & 0x4000 else x)]
                    if v:
                        g[(ax - 16 + x, ay - 16 + y)] = v
        g2 = dict(g)
        for (x, y), v in g.items():
            g2[(x + 16, y)] = v
        ref_start.append(len(pieces))
        emit({0: g2})
    # the turrets' pairs after every other ref (the blocks before keep their
    # numbers: tools/hot_keys.txt)
    emit_steps(True)
    for k, c in tur_alias:
        pair_lut[k] = pair_lut[c]
    # the turrets' second barrels by angle (src/xsp_fast.s h_tur_*): the original
    # (fn_15ee8) puts barrel i at angle a + o_i: x + D_3e3bc[a + o_i + K], y + Dy[a + o_i]
    # (1/64 px), PCG base + D_3dbbc[a] + k, flips info ^ D_3dbbc[a] + 2; tur_tab[flips][site]
    # [index] = the pair's ref (0: none), index = the angle as the second call has it
    # in a1, tur_pcg[site][index] = the PCG it is for
    tur_tab = [0] * (4 * 4 * 256); tur_pcg = [0] * (4 * 256)
    if sz2x:
        from xfile import XFile
        im = XFile(sz2x).image
        sw = lambda a: struct.unpack_from('>h', im, a)[0]
        for site, (o1, o2, dy_tab, kx, k, base, by_a2) in enumerate(TURRET_SITES):
            for a in range(256):
                pcg_ = base + sw(0x3dbbc + a * 4) + k
                a1_, a2_ = (a + o1) & 255, (a + o2) & 255
                idx = a2_ if by_a2 else a
                tur_pcg[site * 256 + idx] = pcg_ & 255
                if not TURRET_LO <= pcg_ <= TURRET_HI:
                    continue
                dx = (sw(0x3e3bc + (a2_ + kx) * 4) - sw(0x3e3bc + (a1_ + kx) * 4)) / 64
                dy = (sw(dy_tab + a2_ * 4) - sw(dy_tab + a1_ * 4)) / 64
                for f in range(4):                  # bit 0: H flip, bit 1: V flip (info >> 14)
                    ndx = math.floor((-dx if f & 1 else dx) + 0.5)
                    ndy = math.floor((-dy if f & 2 else dy) + 0.5)
                    if abs(ndx) > 16 or abs(ndy) > 16:
                        continue
                    r = pair_lut[(TURRET_ROW + pcg_ - TURRET_LO) * 33 * 33 + (ndy + 16) * 33 + ndx + 16]
                    if r:
                        tur_tab[(f * 4 + site) * 256 + idx] = pair_ref + r - 1
    idx_freq.clear(); idx_freq.update(freq_kept)
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

    # ---- large enemies drawn on plane A (src/plane.c): pre-composited images
    # of their refs as drawn by the game, anchored at the call position
    # (x >> 6, y >> 6); tiles in MD colour numbers of one MD palette.
    pimg = []; ptiles = []; pmaps = []; pgroups = []
    for group in PLANE_GROUPS:
        gtiles = TileSet(); first = len(pimg)
        for parts in group:
            pix = {}; mdpal = None
            for rname, ri, dx, objpal, *opt in parts:
                dy = opt[0] if opt else 0
                hf = len(opt) > 1 and opt[1]
                frm, off, cnt, base = ref_src[ref_first[rname] + ri]
                ax = ay = 0
                for j in range(cnt):
                    vx, vy, pt, info = struct.unpack_from('>hhhH', frm, off + j * 8)
                    ax += vx; ay += vy
                    xp = ((info >> 8) & 15) ^ objpal
                    assert mdpal in (None, slot[xp]), 'plane image with two MD palettes'
                    mdpal = slot[xp]
                    px = pcg16(pcg, pt + base)
                    for y in range(16):
                        for x in range(16):
                            v = px[(15 - y if info & 0x8000 else y) * 16 + (15 - x if info & 0x4000 else x)]
                            if v:
                                X = ax - 16 + x
                                if hf:          # the ref mirrored (as src/xsp_fast.s: -dx - 16 - w)
                                    X = -X - 17
                                pix[(X + dx, ay - 16 + y + dy)] = lut[xp][v] & 15
            xs = [p[0] for p in pix]; ys = [p[1] for p in pix]
            ox, oy = min(xs), min(ys)
            if parts[0][0] in ('obj20A', 'obj200'):  # (cells on an 8 px grid from the centre: the
                ox &= ~7; oy &= ~7          # battleship's mirrored wing reuses the tiles flipped,
                                            # obj200's poses share the body's)
            w = (max(xs) - ox) // 8 + 1; h = (max(ys) - oy) // 8 + 1
            moff = len(pmaps)
            for ty in range(h):
                for tx in range(w):
                    t = [pix.get((ox + tx * 8 + x, oy + ty * 8 + y), 0) for y in range(8) for x in range(8)]
                    pmaps.append(add_flipped(gtiles, t) if any(t) else 0)
            pimg.append((ox, oy, w, h, moff, 2 + mdpal, len(pgroups)))
        pgroups.append((len(ptiles), len(gtiles.tiles), first, len(pimg) - first))
        ptiles += gtiles.tiles
    print('plane images:', [(i[2], i[3]) for i in pimg], 'group tiles:', [g[1] for g in pgroups])

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
    arr('u16', 'pair_lut', pair_lut, 33)
    arr('u16', 'tur_tab', tur_tab)
    arr('u8', 'tur_pcg', tur_pcg)
    arr('u16', 'ref_start', ref_start)
    # bounding box of each ref's pieces (x0, y0, x1, y1; x1 / y1 exclusive), for culling
    box = []
    for r in range(nref):
        ps = pieces[ref_start[r]:ref_start[r + 1]]
        if not ps:
            box += [0, 0, 0, 0]
            continue
        box += [min(p[0] for p in ps), min(p[1] for p in ps),
                max(p[0] + (((p[2] >> 2) & 3) + 1) * 8 for p in ps), max(p[1] + ((p[2] & 3) + 1) * 8 for p in ps)]
    arr('s16', 'ref_box', box)
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
    L.append(f'const PlaneImg plane_img[{len(pimg)}] = {{')
    for ox, oy, w, h, moff, pal, g in pimg:
        L.append(f'  {{{ox},{oy},{w},{h},{moff},{pal},{g}}},')
    L.append('};')
    L.append(f'const PlaneGroup plane_group[{len(pgroups)}] = {{')
    for t0, nt, i0, ni in pgroups:
        L.append(f'  {{{t0},{nt},{i0},{ni}}},')
    L.append('};')
    arr('u16', 'plane_map', pmaps)
    pt = b''.join(ptiles)
    L.append(f'const u32 plane_tiles[{len(pt) // 4}] = {{')
    fw = struct.unpack(f'>{len(pt) // 4}I', pt)
    for i in range(0, len(fw), 8):
        L.append('  ' + ','.join(f'0x{v:08x}' for v in fw[i:i + 8]) + ',')
    L.append('};')
    arr('u8', 'pal_lut', sum(lut, []), 32)
    # blocks pre-converted to a palette in ROM, loaded by DMA straight from
    # ROM instead of converted by the CPU (src/xsp.c cache_miss): the
    # (block, palette) pairs stage 1 loads (tools/hot_keys.txt, from
    # tools/uctrace.py --keys-dump) and the player's bomb (expl2 / expl3 /
    # flare refs, BOMB_PALS), whose blocks also get a meshed copy (plane A)
    mkeys = sorted({(b << 4) | ((px ^ op) & 15) for r in BOMB_REFS for op in BOMB_PALS
                    for dx, dy, sz, b, px in pieces[ref_start[r]:ref_start[r + 1]]})
    hot = set()
    hp = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hot_keys.txt')
    if os.path.exists(hp):
        for line in open(hp):
            if line.strip() and not line.startswith('#'):
                k = int(line.split()[0], 16)
                if (k >> 4) < len(blocks.table):
                    hot.add(k)
    pkeys = sorted(set(mkeys) | hot)
    ptiles_rom = bytearray(); poff = []; moff = []
    for mesh, keys, offs in ((0, pkeys, poff), (1, mkeys, moff)):
        for k in keys:
            off, w, h = blocks.table[k >> 4]
            offs.append(len(ptiles_rom) // 32)
            t = lut[k & 15]
            for r in range(w * h * 8):
                row = bytes(t[v] for v in blocks.data[off * 32 + r * 4:off * 32 + r * 4 + 4])
                m = 0xFF if not mesh else 0xF0 if r % 2 == 0 else 0x0F
                ptiles_rom += bytes(v & m for v in row)
    assert len(ptiles_rom) // 32 < 0x8000
    arr('u16', 'pre_keys', pkeys)
    arr('u16', 'pre_off', poff)
    arr('u16', 'mesh_keys', mkeys)
    arr('u16', 'mesh_off', moff)
    fw = struct.unpack(f'>{len(ptiles_rom) // 4}I', bytes(ptiles_rom))
    L.append(f'const u32 pre_tiles[{len(fw)}] = {{')
    for i in range(0, len(fw), 8):
        L.append('  ' + ','.join(f'0x{v:08x}' for v in fw[i:i + 8]) + ',')
    L.append('};')
    open(os.path.join(out, 'spr.c'), 'w').write('\n'.join(L) + '\n')
    H = ['/* generated by tools/convert_sprites.py - do not edit */', '#ifndef SPR_H', '#define SPR_H',
         '#include "types.h"',
         '/* dx, dy: top-left relative to the sprite position (x >> 6, y >> 6);',
         '   size: MD sprite size bits (w-1)<<2|(h-1), bit 4 = H flip, bit 5 = V flip */',
         'typedef struct { s16 dx, dy; u8 size, palx; u16 block; } SprPiece;',
         'typedef struct { u32 tile; u16 ntiles; } SprBlock;  /* tile = offset in spr_tiles (32-byte units) */',
         f'#define NPCG {npcg}', f'#define NREF {nref}', f'#define NBLOCK {len(blocks.table)}',
         f'extern const u16 pcg_block[{npcg}];', f'extern const u16 ref_start[{nref + 1}];',
         f'extern const s16 ref_box[{nref * 4}];   /* x0, y0, x1, y1 of the pieces (x1, y1 exclusive) */',
         f'extern const SprPiece ref_pieces[{len(pieces)}];', f'extern const SprBlock spr_blocks[{len(blocks.table)}];',
         'extern const u16 spr_cram[32];', 'extern const u8 pal_slot[16];', 'extern const u8 pal_lut[16 * 256];',
         'extern const u8 spr_tiles[];', 'extern const u32 text_font[1024];',
         f'#define TEXT_PAL {slot[TEXT_XPAL] + 2}',
         '/* plane A enemies: image = tile map w x h (plane_map, local tile + 1, 0 = empty) whose',
         '   top-left is (ox, oy) from the call position; group = tiles loaded together */',
         'typedef struct { s16 ox, oy; u8 w, h; u16 map; u8 pal, group; } PlaneImg;',
         'typedef struct { u16 tile, ntiles; u8 img, nimg; } PlaneGroup;',
         f'#define REF_EMPTY {[i for i in range(nref) if ref_start[i + 1] == ref_start[i]][0]:#x}   /* a ref without pieces */',
         f'#define PLANE_NIMG {len(pimg)}', f'#define PLANE_MAX_TILES {max(g[1] for g in pgroups)}',
         f'extern const PlaneImg plane_img[{len(pimg)}];', f'extern const PlaneGroup plane_group[{len(pgroups)}];',
         f'extern const u16 plane_map[{len(pmaps)}];', f'extern const u32 plane_tiles[{len(pt) // 4}];',
         '/* blocks pre-converted in ROM (sorted keys block << 4 | palette, tile offsets in pre_tiles):',
         '   plain (pre_*) and meshed (mesh_*: the bomb) */',
         f'#define PRE_NKEYS {len(pkeys)}', f'extern const u16 pre_keys[{len(pkeys)}];',
         f'extern const u16 pre_off[{len(pkeys)}];', f'#define MESH_NKEYS {len(mkeys)}',
         f'extern const u16 mesh_keys[{len(mkeys)}];', f'extern const u16 mesh_off[{len(mkeys)}];',
         f'extern const u32 pre_tiles[{len(fw)}];',
         '#endif', '']
    open(os.path.join(out, 'spr.h'), 'w').write('\n'.join(H))
    open(os.path.join(out, 'spr_def.inc'), 'w').write(f'| generated by tools/convert_sprites.py\n'
                                                      f'    .set NPCG, {npcg}\n    .set NREF, {nref}\n'
                                                      f'    .set VLASER_REF, {vlaser_ref}\n    .set PAIR_REF, {pair_ref}\n    .set LBEAM_REF, {lbeam_ref}\n    .set TURRET_LO, {TURRET_LO}\n    .set TURRET_HI, {TURRET_HI}\n    .set TURRET_ROW, {TURRET_ROW}\n    .set TEXT_PAL, {slot[TEXT_XPAL] + 2}\n')
    print(f'pre-converted: {len(pkeys)} keys + {len(mkeys)} meshed, {len(ptiles_rom) // 1024} KB')
    print(f'sprites: {npcg} PCGs, {nref} refs, {len(pieces)} pieces, {len(blocks.table)} blocks, '
          f'{len(blocks.data) // 1024} KB tiles; palette slots {"".join(str(s) for s in slot)}')

if __name__ == '__main__':
    main(*sys.argv[1:4])
