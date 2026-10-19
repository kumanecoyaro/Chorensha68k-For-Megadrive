"""Helpers for Chorensha68k X68000 graphics data (XSP_DAT)."""
import struct

def load_pal(path):
    """X68000 palette file: 256 big-endian GRB555+I words -> list of (r,g,b) 0..255."""
    d = open(path, 'rb').read()
    out = []
    for i in range(len(d) // 2):
        w = struct.unpack_from('>H', d, i * 2)[0]
        g = (w >> 11) & 31; r = (w >> 6) & 31; b = (w >> 1) & 31
        out.append((r * 255 // 31, g * 255 // 31, b * 255 // 31))
    return out

def tile8(data, idx):
    """8x8 4bpp tile (32 bytes, high nibble = left pixel) -> 64 pixel indices."""
    t = data[idx * 32: idx * 32 + 32]
    px = []
    for b in t:
        px += [b >> 4, b & 15]
    return px

def pcg16(data, idx):
    """16x16 X68000 sprite PCG (128 bytes: TL, BL, TR, BR 8x8 blocks) -> 256 pixels."""
    px = [0] * 256
    for blk, (bx, by) in enumerate(((0, 0), (0, 8), (8, 0), (8, 8))):
        t = tile8(data, idx * 4 + blk)
        for y in range(8):
            for x in range(8):
                px[(by + y) * 16 + bx + x] = t[y * 8 + x]
    return px

def load_chips(path):
    """map.chp: 8-byte header (w,h longs) + 256 chips of 4x4 BG words."""
    d = open(path, 'rb').read()
    w, h = struct.unpack_from('>II', d, 0)
    n = (len(d) - 8) // (w * h * 2)
    return [[struct.unpack_from('>H', d, 8 + (c * w * h + i) * 2)[0] for i in range(w * h)] for c in range(n)]

def load_map(path):
    """map.bg?: 16-byte header (?,?,w,h longs) + w*h chip bytes."""
    d = open(path, 'rb').read()
    _, _, w, h = struct.unpack_from('>IIII', d, 0)
    return w, h, d[16:16 + w * h]

def chip_pixels(chip, bgpcg):
    """Render one 32x32 chip -> 1024 8-bit color indices (pal*16+n, 0 = transparent)."""
    out = [0] * 1024
    for ty in range(4):
        for tx in range(4):
            wd = chip[ty * 4 + tx]
            pat = wd & 0xff; pal = (wd >> 8) & 15
            hf = wd & 0x4000; vf = wd & 0x8000
            t = tile8(bgpcg, pat)
            for y in range(8):
                for x in range(8):
                    sx = 7 - x if hf else x; sy = 7 - y if vf else y
                    c = t[sy * 8 + sx]
                    out[(ty * 8 + y) * 32 + tx * 8 + x] = (pal << 4 | c) if c else 0
    return out
