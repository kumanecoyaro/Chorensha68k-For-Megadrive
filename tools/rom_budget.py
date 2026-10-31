#!/usr/bin/env python3
"""ROM size report: current build + projection for the whole game.

usage: rom_budget.py BUILD_DIR ROM.bin

Everything sits in ROM already expanded / converted (the X68000 version
unpacks assets.tgz into RAM; the Mega Drive has 64 KB of RAM).
"""
import os, sys, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

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
    gen = os.path.join(build, 'gen')
    xsp = os.path.join(build, 'assets', 'XSP_DAT')
    size = lambda f: os.path.getsize(os.path.join(gen, f))
    rom_size = os.path.getsize(rom)
    raw = os.path.getsize(os.path.join(build, 'game.raw'))
    syms = symsizes(os.path.join(build, 'game.elf'))
    bgm = int(open(os.path.join(gen, 'bgm.h')).read().split('BGM_DATA_SIZE')[1].split()[0])
    spr_tables = sum(syms.get(s, 0) for s in ('pcg_block', 'ref_start', 'ref_pieces', 'spr_blocks', 'pal_lut',
                                               'spr_cram', 'pal_slot', 'text_font'))
    bgset = ('bg_pal_', 'bg_tiles_', 'chip_near_', 'chip_far_', 'bg_far_ix_', 'bg_near_mix_', 'bg_comp_')
    bg = sum(v for k, v in syms.items() if k.startswith(bgset) or k in ('bg_sets', 'pal_data'))
    nsets = int(open(os.path.join(gen, 'assets.h')).read().split('BG_NSETS')[1].split()[0])
    rows = [('original program (SZ2.X text + data, relocated)', size('orig.bin') - 0x4000),
            ('BG maps (map.bg0 / bg1, read by the original code)', 0x4000),
            ('original variables, RAM image', size('orig_ram.bin')),
            ('sprite tiles (all PCGs / refs, 4bpp)', size('spr_tiles.bin')),
            ('sprite tables (pieces, blocks, palette LUTs, font)', spr_tables),
            (f'BG sets: tiles, chip / composite tables ({nsets} stages)', bg),
            ('PCM sound effects (8-bit 15.625 kHz)', size('pcm.bin')),
            ('BGM (13 MDX songs + voices)', bgm)]
    used = sum(v for _, v in rows)
    print('== current build ==')
    for n, v in rows:
        print(f'  {n:52s} {kb(v)}')
    print(f'  {"port code, other tables":52s} {kb(raw - used)}')
    print(f'  image {kb(raw)}  -> ROM file {kb(rom_size)}')

    # ---- projection: BG for the other stages (as many as the sets so far on average)
    extra = (6 - nsets) * bg // nsets
    total = raw + extra
    print('== projection: all stages ==')
    print(f'  {"BG tiles + chips for the other stages (estimate)":52s} {kb(extra)}')
    print(f'  {"total":52s} {kb(total)}  ({total * 100 / LIMIT:.1f}% of 4 MB)')
    print('  -> fits in 4 MB' if total <= LIMIT else '  -> DOES NOT FIT in 4 MB')

if __name__ == '__main__':
    main(*sys.argv[1:3])
