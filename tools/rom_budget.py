#!/usr/bin/env python3
"""ROM size report: current build + projection with every asset pre-expanded.

usage: rom_budget.py BUILD_DIR ROM.bin

The X68000 game unpacks assets.tgz into RAM at start-up; the Mega Drive has
64 KB of RAM, so everything has to sit in ROM already expanded/converted.
"""
import os, sys, struct, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x68gfx import load_chips, load_map

LIMIT = 4 * 1024 * 1024

def kb(n):
    return f'{n / 1024:9.1f} KB'

def symsizes(elf):
    out = subprocess.run(['m68k-linux-gnu-nm', '-S', '--size-sort', elf], capture_output=True, text=True).stdout
    d = {}
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 4:
            d[p[3]] = int(p[1], 16)
    return d

def main(build, rom):
    xsp = os.path.join(build, 'assets', 'XSP_DAT')
    disk = os.path.join(build, 'disk', 'CRS68K')
    rom_size = os.path.getsize(rom)
    raw = os.path.getsize(os.path.join(build, 'game.raw'))
    pcm = os.path.getsize(os.path.join(build, 'gen', 'pcm.bin'))
    syms = symsizes(os.path.join(build, 'game.elf'))
    bgm = int(open(os.path.join(build, 'gen', 'bgm.h')).read().split('BGM_DATA_SIZE')[1].split()[0])
    asset_syms = ['bg_tiles', 'spr_tiles', 'font_tiles', 'chip_near', 'chip_far', 'map_bg0', 'map_bg1',
                  'frame_pieces', 'frame_start', 'pal_data']
    table_syms = ['trig_tab', 'atan_tab', 'rand_tab', 'shotdir_tab', 'power_tab', 'dir_angle_tab', 'pcm_table',
                  'fnum_tab', 'psg_tab', 'tempo_tab', 'z80_driver']
    a = sum(syms.get(s, 0) for s in asset_syms)
    t = sum(syms.get(s, 0) for s in table_syms)
    print('== current build ==')
    print(f'  PCM samples (all {pcm} bytes)   {kb(pcm)}')
    print(f'  BGM (all 13 MDX songs)         {kb(bgm)}')
    print(f'  graphics / maps (stage 1 part) {kb(a)}')
    print(f'  tables from SZ2.X              {kb(t)}')
    print(f'  code + other                   {kb(raw - pcm - bgm - a - t)}')
    print(f'  image {kb(raw)}  -> ROM file {kb(rom_size)} (padded to 128 KB)')

    # ---- projection: every asset converted
    files = os.listdir(xsp)
    size = lambda f: os.path.getsize(os.path.join(xsp, f))
    pcg = sum(size(f) for f in files if f.endswith(('.sp', '.xsp')))
    frm = sum(size(f) for f in files if f.endswith('.frm'))
    ref = sum(size(f) for f in files if f.endswith('.ref'))
    # PCG 16x16 = MD 2x2 sprite (same 4bpp column-major layout).  Palette is
    # chosen per object at run time on the X68000; allow 2 colour-remapped
    # copies of every PCG for the Mega Drive palette groups.
    spr = pcg * 2
    pieces = frm // 8 * 6 + ref // 8 * 2       # SprPiece (6 bytes) + start index
    # BG: both chip sets, every (pattern, palette) combination that is used
    bg = 0
    for chp, pcgf in (('map.chp', 'bg_pcg.sp'), ('end.chp', 'end_pcg.sp')):
        chips = load_chips(os.path.join(xsp, chp))
        combos = {(w & 0xFF, (w >> 8) & 15) for c in chips for w in c}
        bg += len(combos) * 32 + len(chips) * 16 * 2 * 2     # tiles + chip tables for 2 planes
    maps = sum(len(load_map(os.path.join(xsp, m))[2]) for m in ('map.bg0', 'map.bg1'))
    pals = sum(1 for f in files if f.endswith('.pal')) * 4 * 32
    code = 0x3C104                             # SZ2.X text: 68000 -> 68000 port, similar size
    tables = 0xDEB0 + 0x8000                   # SZ2.X data section + trig table already used
    rows = [('PCM (all 62 entries, 8-bit 15.625 kHz)', pcm),
            ('sprite PCG tiles (all .sp/.xsp, x2 palettes)', spr),
            ('sprite frame tables (all .ref/.frm)', pieces),
            ('BG tiles + chip tables (map / end)', bg),
            ('BG maps (bg0 / bg1)', maps),
            ('palettes', pals),
            ('BGM (13 MDX songs + voices, measured)', bgm),
            ('game code (SZ2.X text size)', code),
            ('data tables (SZ2.X data + trig)', tables)]
    total = sum(v for _, v in rows)
    print('== projection: whole game, assets pre-expanded in ROM ==')
    for n, v in rows:
        print(f'  {n:46s} {kb(v)}')
    print(f'  {"total":46s} {kb(total)}  ({total * 100 / LIMIT:.1f}% of 4 MB)')
    print('  -> fits in 4 MB' if total <= LIMIT else '  -> DOES NOT FIT in 4 MB')

if __name__ == '__main__':
    main(*sys.argv[1:3])
