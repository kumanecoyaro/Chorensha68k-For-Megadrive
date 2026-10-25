#!/usr/bin/env python3
"""Relocate the original SZ2.X game code so it runs natively on the Mega Drive.

usage: relocate.py SZ2.X LISTING XSP_DAT_DIR OUT_DIR

The X68000 and the Mega Drive share the 68000, so the game logic of SZ2.X
(stage scripts, enemies, player, collision, ...) runs unchanged.  Only the
memory map differs:

  * text + data (tables, code) go to ROM at ORIG_ROM (address a -> ORIG_ROM + a)
  * the parts of BSS the game logic uses, the few data-section variables it
    writes, and two small library routines that keep their state in their own
    text section (collision grid, random table index) go to RAM (RAM_MAP)
  * every absolute address in the program is listed in the .X relocation
    table, so rewriting those longwords moves all references
  * calls into the X68000 libraries (XSP sprites, IOCS, text VRAM, MCDRV
    sound, graphics-VRAM BG drawing) are redirected: the first 6 bytes of the
    library function become `jmp HOOK_BASE + 6*i`, and the hook table
    (src/hooks.s) jumps to the Mega Drive replacement.

Outputs OUT_DIR/orig.bin (ROM image at ORIG_ROM), orig_ram.bin (initial
contents of the RAM area at ORIG_RAM), orig.h (addresses for C / asm) and
hooks.inc (hook table entries, in index order).
"""
import os, re, sys, struct, bisect, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xfile import XFile

ORIG_ROM = 0x10000
ORIG_RAM = 0xFF0000
HOOK_BASE = 0x200
POOL_OBJS = 128                       # object pool (X68000: 383)

# (orig lo, orig hi, comment): mapped to RAM in this order, contiguously
RAM_MAP = [
    (0x49fb4, 0x4d39e + POOL_OBJS * 0x80, 'globals, shots, object pool'),
    (0x5931e, 0x5939e, 'object list tail / stage / player state'),
    (0x8421a, 0x8424a, 'frame skip state'),
    (0x90252, 0x921c4, 'BG state, player shot state, misc, the floor enemy tables (fn_15ee8: to $921c4)'),
    (0x82004, 0x8201a, 'score / hi-score digit cache (fn_022b6 / fn_02336)'),
    (0x46860, 0x46868, 'class $40 update/init (patched per stage by fn_044a2)'),
    (0x4898c, 0x48994, 'palette animation state (fn_05a3a)'),
    (0x2f3cc, 0x2f922, 'collision routines + grid (PC-relative data)'),
    (0x345be, 0x349e2, 'random number routine + table (PC-relative index)'),
]
# ROM blobs that replace BSS buffers the original fills from files
ROM_MAP = [
    (0x8c252, 0x90252, 'map.bg0 + map.bg1 (B_8c252) -> rom_maps'),
]

# hooked library functions: (address, C symbol, kind)
#   kind 'c': stack arguments (XC calling convention) -> plain C function
#   kind 'r': register arguments -> asm wrapper in src/hooks.s (symbol h_<name>)
HOOKS = [
    # XSP sprite library
    (0x2c9fe, 'xsp_set', 'r'),          # d0 x, d1 y, d2 pcg, d3 info
    (0x2ca2c, 'xsp_set_obj', 'r'),      # a0 -> {x, y, pcg, info}
    # (fn_2ca5a = movem.w (a0),d0-d3 and falls into fn_2ca5e: stays original)
    (0x2ca5e, 'xobj_set', 'r'),         # d0 x, d1 y, d2 ref, d3 info
    (0x2cbda, 'x_vsync', 'c'),
    (0x2cf3a, 'x_xsp_out', 'c'),
    (0x2cf40, 'x_xsp_out0', 'c'),       # (same without BG scroll: title screen)
    (0x2cf24, 'x_nop', 'c'),            # XSP mode
    # input (IOCS _BITSNS / _JOYGET wrappers)
    (0x39dac, 'x_bitsns', 'c'),
    (0x39e16, 'x_joydir', 'c'),
    (0x39e58, 'x_joybtn', 'c'),
    (0x39e7a, 'x_joyget', 'c'),
    # replay (removed): the recorder / player input functions read the pad
    # directly, replay start / end / buffer setup do nothing, B_4a0a4 stays 0
    (0x00f26, 'x_input_dir', 'c'),      # replay recorder / player
    (0x01048, 'x_input_btn', 'c'),      # replay recorder / player (buttons)
    (0x00f06, 'x_nop', 'c'),
    (0x01036, 'x_nop', 'c'),
    (0x00c9c, 'x_nop', 'c'),
    # screen / misc IOCS
    (0x3975a, 'x_screen_clear', 'c'),   # IOCS $B2 (graphic screen on / off): title, game start
    (0x00c48, 'x_nop', 'c'),            # replay buffer allocation (replay removed)
    (0x39d92, 'x_nop', 'c'),
    (0x2ff62, 'x_wait', 'c'),           # wait n+1 vertical blanks (title, game over, pause)
    (0x39dda, 'x_nop', 'c'),
    # palettes (graphic / sprite palette registers)
    (0x05224, 'x_pal_upload', 'c'),
    (0x0479a, 'x_nop', 'c'),            # palette fade into the graphic palette buffer (not shown yet)
    (0x05a3a, 'x_pal_anim', 'c'),
    (0x051b8, 'x_pal_load', 'c'),
    # BG: file loading and graphic-VRAM chip drawing
    (0x053ba, 'x_bg_load', 'c'),
    (0x05574, 'x_bg_full', 'c'),        # full redraw (src/hooks.s orig_05574 runs the original)
    (0x301f6, 'x_bg_chip', 'c'),
    (0x303a2, 'x_bg_chip', 'c'),
    # text VRAM (the number / string printers above it stay original)
    (0x2f06a, 'x_text_putc', 'c'),
    (0x2f2aa, 'x_text_clear', 'c'),
    (0x2f30a, 'x_nop', 'c'),            # text scroll
    # sound (MCDRV / ADPCM)
    (0x034ae, 'x_se_request', 'c'),
    (0x03576, 'x_nop', 'c'),
    (0x03500, 'x_nop', 'c'),
    (0x032f2, 'x_nop', 'c'),
    (0x03590, 'x_bgm_play', 'c'),
    (0x03872, 'x_bgm_fade', 'c'),
    (0x03882, 'x_nop', 'c'),
    (0x035ec, 'x_nop', 'c'),
    # game over: ranking / name entry (the original, src/hooks.s orig_0132c; its
    # graphic screen boxes and VBlank count wait do nothing here), title
    (0x0132c, 'x_game_over', 'c'),
    (0x39764, 'x_nop', 'c'),            # (CRTC / graphic screen)
    (0x39816, 'x_nop', 'c'),            # IOCS $B1 APAGE
    (0x39820, 'x_nop', 'c'),            # IOCS $B4 WINDOW
    (0x39834, 'x_nop', 'c'),            # IOCS $BA FILL
    (0x39852, 'x_nop', 'c'),            # IOCS $B5 WIPE
    (0x2cbb8, 'x_nop', 'c'),            # XSP vsync count wait (x_xsp_out0 paces)
    (0x01106, 'x_nop', 'c'),            # write ETC_DAT/SCORE.DAT
    (0x02dac, 'x_title_menu', 'c'),     # title menu (src/title.c)
    (0x3966a, 'x_nop', 'c'),            # printf (error messages)
    (0x39750, 'x_nop', 'c'),
]

# particles (parts flying off destroyed enemies, sparks): every allocation is
# refused (`jsr fn_040b6` -> `jsr h_alloc_none`)
PARTICLE_INITS = [
    0x0ec0e, 0x0ece0, 0x0edb2,                  # $81-$83: debris.sp parts
    0x10a1a, 0x10abc, 0x10b7c,                  # $122-$124: sparks / debris
]

# immediate patches: (address, old bytes hex, new bytes hex, comment)
def patches():
    n = POOL_OBJS
    return [
        (0x0407c, '0180', f'{n + 1:04x}', 'object pool size (cmp.w #$180)'),
        (0x04082, '017f', f'{n:04x}', 'free object count (move.w #$17f)'),
        # player Y limit: the X68000 screen is 256 lines, the Mega Drive shows 224
        (0x0604a, '3f00', '3700', 'player y limit (cmp.w #$3f00,d1)'),
        (0x06052, '3f00', '3700', 'player y limit (move.w #$3f00,B_4cca0)'),
        # missile smoke (class $121, fn_10972 / L109d0: 8 puff frames over $80 / rate
        # frames): the frames twice as fast, gone at $40: the trail half as long
        # (puffs as dense as before; sprites halved)
        (0x109e6, 'e840', 'e640', 'smoke: frame = age >> 3 (asr.w #4 -> #3)'),
        (0x10a14, '0080', '0040', 'smoke: gone at age $40 (cmp.w #$80)'),
        # stages 1-4 are converted so far: after the stage 4 clear (state
        # $210) back to stage 1 (addq.w #1,B_4a0ee -> subq.w #3; src/bg.c restarts the map)
        (0x0a4de, '5279', '5779', 'stage 4 -> stage 1 (addq.w #1,abs.l -> subq.w #3)'),
        # fn_056ae skips a chip equal to the one already in the page cell; with
        # both layers on one plane (src/bg.c) the far chip under it may differ
        (0x057f0, '6774', '4e71', 'BG: always draw the chip (beq.b L05866 -> nop)'),
        # L03e60: "exit" stays on the title (bne.b L03f5c -> bra.b)
        (0x03f46, '6614', '6014', 'title: exit does not quit'),
        # fn_0132c: a new first place jumps into the replay saver (L00dba, past
        # the fn_00c9c hook: file output) -> the normal end instead
        (0x01fb2, '660002f8', '600002f8', 'ranking: no replay save (bne.w L022ac -> bra.w)'),
        # the bomb's circles (and the player's hit flash): one frame each, no
        # animation, and not the big expl2 frames $B8 / $B9 (96 / 128 px): no new
        # tiles to load every few frames.  D_48ddc: expl3 ring frame ($BA + n)
        # by age (fn_10154), D_48dfa: expl2 frame ($B5 + n) by age (fn_0ff3a)
        (0x48ddc, '000102030405000000000101010102020202030303030404040405050505', '02' * 30,
         'circles: expl3 ring = $BC always'),
        (0x48dfa, '000001010202030304040403030302020201010100000000', '02' * 24,
         'circles: expl2 = $B7 always (no $B8 / $B9)'),
    ]

def main(xpath, lst, xspdir, out):
    x = XFile(xpath)
    img = bytearray(x.image)
    os.makedirs(out, exist_ok=True)

    # ---- RAM layout
    ram = []
    a = ORIG_RAM
    for lo, hi, c in RAM_MAP:
        ram.append((lo, hi, a, c))
        a += hi - lo
        a = (a + 1) & ~1
    ram_size = a - ORIG_RAM
    assert ram_size <= 0xA000, hex(ram_size)

    def remap(v):
        for lo, hi, dst, _ in ram:
            if lo <= v < hi:
                return dst + v - lo
        for lo, hi, _ in ROM_MAP:
            if lo <= v < hi:
                return None             # resolved by the linker (see orig.h / roms)
        if v < x.data_end:
            return ORIG_ROM + v
        return -1

    # ---- function index (for reports)
    fst = []
    for ln in open(lst):
        m = re.match(r'^fn_([0-9a-f]+):', ln)
        if m:
            fst.append(int(m.group(1), 16))
    fst.sort()
    fn_of = lambda p: 'fn_%05x' % fst[bisect.bisect_right(fst, p) - 1]

    hooked = {h[0] for h in HOOKS}
    unmapped = collections.defaultdict(set)
    rom_refs = []
    for r in x.relocs:
        v = x.l(r)
        n = remap(v)
        if n is None:
            rom_refs.append((r, v))
            continue
        if n == -1:
            unmapped[fn_of(r)].add(v)
            n = ORIG_ROM + v            # unreachable code: harmless ROM address
        struct.pack_into('>I', img, r, n)

    # ROM_MAP buffers are stored right after the image: B_8c252 holds the two
    # maps (layer 0 = map.bg0, layer 1 = map.bg1, $2000 bytes each, 16-byte header)
    rom_blob_base = ORIG_ROM + ((len(img) + 0xFF) & ~0xFF)
    blob = b''
    for f in ('map.bg0', 'map.bg1'):
        d = open(os.path.join(xspdir, f), 'rb').read()[16:]
        assert len(d) == 0x2000, (f, len(d))
        blob += d
    for r, v in rom_refs:
        lo, hi, _ = next(m for m in ROM_MAP if m[0] <= v < m[1])
        struct.pack_into('>I', img, r, rom_blob_base + v - lo)

    for addr, old, new, c in patches():
        ob = bytes.fromhex(old)
        assert img[addr:addr + len(ob)] == ob, (hex(addr), img[addr:addr + 4].hex(), c)
        nb = bytes.fromhex(new)
        img[addr:addr + len(nb)] = nb

    # ---- hooks
    names = ['h_alloc_none']
    hs = sorted(h[0] for h in HOOKS)
    for a, b in zip(hs, hs[1:]):
        assert b - a >= 6, f'hook patches overlap at ${a:x} / ${b:x}'
    for addr, name, kind in HOOKS:
        sym = name if kind == 'c' else 'h_' + name
        if sym not in names:
            names.append(sym)
        i = names.index(sym)
        struct.pack_into('>HI', img, addr, 0x4EF9, HOOK_BASE + 6 * i)

    # particle allocations: retarget the first jsr fn_040b6 of each init function
    ai = HOOK_BASE + 6 * names.index('h_alloc_none')
    for f in PARTICLE_INITS:
        for a in range(f, f + 0x40, 2):
            if img[a:a + 2] == b'\x4e\xb9' and struct.unpack_from('>I', img, a + 2)[0] == ORIG_ROM + 0x40b6:
                struct.pack_into('>I', img, a + 2, ai)
                break
        else:
            raise SystemExit(f'no jsr fn_040b6 in ${f:x}')

    # ---- RAM initial image (data vars / routines copied to RAM)
    rimg = bytearray(ram_size)
    for lo, hi, dst, _ in ram:
        if lo < x.data_end:
            rimg[dst - ORIG_RAM:dst - ORIG_RAM + hi - lo] = img[lo:hi]
    # the ranking (B_49fb4, 10 x 24 bytes: score, stage, name) as the original
    # loads it from ETC_DAT/SCORE.DAT at start (not saved: no backup RAM)
    sd = open(os.path.join(os.path.dirname(xpath), 'ETC_DAT', 'SCORE.DAT'), 'rb').read()
    a = remap(0x49fb4) - ORIG_RAM
    rimg[a:a + len(sd)] = sd

    pad = rom_blob_base - ORIG_ROM - len(img)
    open(os.path.join(out, 'orig.bin'), 'wb').write(bytes(img) + bytes(pad) + blob)
    open(os.path.join(out, 'orig_ram.bin'), 'wb').write(rimg)
    with open(os.path.join(out, 'hooks.inc'), 'w') as f:
        f.write('| generated by tools/relocate.py - do not edit\n')
        for s in names:
            f.write(f'    .word 0x4EF9\n    .long {s}\n')
        f.write(f'    .globl ORIG_VBLCNT\n    .set ORIG_VBLCNT, 0x{remap(0x4a0cc):x}\n')
    h = ['/* generated by tools/relocate.py - do not edit */', '#ifndef ORIG_H', '#define ORIG_H',
         f'#define ORIG_ROM 0x{ORIG_ROM:x}', f'#define ORIG_RAM 0x{ORIG_RAM:x}',
         f'#define ORIG_RAM_SIZE 0x{ram_size:x}', f'#define ORIG_IMG_SIZE 0x{len(img):x}',
         f'#define ORIG_BLOB 0x{rom_blob_base:x}', f'#define POOL_OBJS {POOL_OBJS}',
         '/* original address -> Mega Drive address (constant-folds) */',
         '#define OADDR(a) ( \\']
    for lo, hi, dst, c in ram:
        h.append(f'  ((a) >= 0x{lo:x} && (a) < 0x{hi:x}) ? (0x{dst:x} + (a) - 0x{lo:x}) : /* {c} */ \\')
    for lo, hi, c in ROM_MAP:
        h.append(f'  ((a) >= 0x{lo:x} && (a) < 0x{hi:x}) ? (0x{rom_blob_base:x} + (a) - 0x{lo:x}) : \\')
    h.append(f'  (ORIG_ROM + (a)))')
    h += ['#define OW(a)  (*(volatile s16 *)OADDR(a))', '#define OUW(a) (*(volatile u16 *)OADDR(a))',
          '#define OL(a)  (*(volatile s32 *)OADDR(a))', '#define OB(a)  (*(volatile u8 *)OADDR(a))',
          '#define OFN(a) ((void *)(ORIG_ROM + (a)))', '#endif', '']
    open(os.path.join(out, 'orig.h'), 'w').write('\n'.join(h))

    print(f'orig image {len(img)} bytes at ${ORIG_ROM:x}, RAM {ram_size} bytes at ${ORIG_RAM:x}, '
          f'{len(names)} hook entries, map blob at ${rom_blob_base:x}')
    for lo, hi, dst, c in ram:
        print(f'  ${lo:05x}-${hi:05x} -> ${dst:06x}  {c}')
    if unmapped:
        print('BSS references outside the mapped ranges (must be unreachable / hooked):')
        for f in sorted(unmapped):
            print(f'  {f}: ' + ' '.join(f'${v:x}' for v in sorted(unmapped[f])))

if __name__ == '__main__':
    main(*sys.argv[1:5])
