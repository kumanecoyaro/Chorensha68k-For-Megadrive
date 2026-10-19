#!/usr/bin/env python3
"""Recursive-descent disassembler for Human68k .X files (capstone based).

Writes an annotated listing: labels for branch targets / relocated pointers,
Human68k DOS ($FFxx) and IOCS (trap #15) call names, X68000 I/O register names.
"""
import sys, struct, re
import capstone
from capstone import m68k as M
from xfile import XFile

DOS = {0xff00:'_EXIT',0xff01:'_GETCHAR',0xff02:'_PUTCHAR',0xff06:'_INPOUT',0xff07:'_INKEY',
 0xff09:'_PRINT',0xff0b:'_KEYSNS',0xff0c:'_KFLUSH',0xff1b:'_FGETC',0xff1e:'_FPUTS',0xff20:'_SUPER',
 0xff21:'_FNCKEY',0xff23:'_CONCTRL',0xff25:'_INTVCS',0xff27:'_GETTIM2',0xff2a:'_GETDATE',
 0xff35:'_INTVCG',0xff3c:'_CREATE',0xff3d:'_OPEN',0xff3e:'_CLOSE',0xff3f:'_READ',0xff40:'_WRITE',
 0xff41:'_DELETE',0xff42:'_SEEK',0xff44:'_IOCTRL',0xff48:'_MALLOC',0xff49:'_MFREE',0xff4a:'_SETBLOCK',
 0xff4b:'_EXEC',0xff4c:'_EXIT2',0xff4e:'_FILES',0xff4f:'_NFILES',0xff57:'_FILEDATE',0xfff5:'_CHANGE_PR'}
IOCS = {0x00:'_B_KEYINP',0x01:'_B_KEYSNS',0x02:'_B_SFTSNS',0x04:'_BITSNS',0x10:'_TEXTGET',
 0x0e:'_SET_LED',0x0c:'_TGUSEMD?',0x10:'_CRTMOD?',
 0x20:'_B_PUTC',0x21:'_B_PRINT',0x22:'_B_COLOR',0x23:'_B_LOCATE',0x2f:'_B_CONSOL',
 0x3b:'_JOYGET',0x60:'_ADPCMOUT',0x67:'_ADPCMMOD',0x6a:'_OPMINTST',0x6b:'_TIMERDST',0x6c:'_VDISPST',
 0x6d:'_CRTCRAS',0x6e:'_HSYNCST',0x7f:'_ONTIME',0x80:'_B_INTVCS',0x81:'_B_SUPER',0x90:'_G_CLR_ON',
 0xac:'_SYS_STAT',0xae:'_OS_CURON',0xaf:'_OS_CUROF',0xc0:'_SP_INIT',0xc1:'_SP_ON',0xc2:'_SP_OFF',
 0xc3:'_SP_CGCLR',0xc4:'_SP_DEFCG',0xc5:'_SP_GTPCG',0xc6:'_SP_REGST',0xc7:'_SP_REGGT',0xc8:'_BGSCRLST',
 0xc9:'_BGSCRLGT',0xca:'_BGCTRLST',0xcb:'_BGCTRLGT',0xcc:'_BGTEXTCL',0xcd:'_BGTEXTST',0xce:'_BGTEXTGT',
 0xcf:'_SPALET',0x10:'_CRTMOD',0x11:'_CONTRAST',0x13:'_TPALET',0x14:'_TPALET2',0x15:'_TCOLOR',
 0x1e:'_MS_INIT?',0x84:'_MS_INIT',0x85:'_MS_CURON',0x86:'_MS_CUROF',0xff:'_ABORTJOB'}

HW = [(0xe80000,0xe80030,'CRTC'),(0xe82000,0xe82200,'GPAL'),(0xe82200,0xe82400,'TSPAL'),
      (0xe82400,0xe82700,'VCTRL'),(0xe84000,0xe84100,'DMAC'),(0xe88000,0xe88040,'MFP'),
      (0xe8a000,0xe8c000,'RTC'),(0xe8e000,0xe90000,'SYSPORT'),(0xe90000,0xe92000,'OPM'),
      (0xe92000,0xe94000,'ADPCM'),(0xe9a000,0xe9c000,'PPI(JOY)'),
      (0xeb0000,0xeb0400,'SPRREG'),(0xeb0800,0xeb0812,'BGCTRL'),(0xeb8000,0xebc000,'PCG'),
      (0xebc000,0xebe000,'BG0TXT'),(0xebe000,0xec0000,'BG1TXT'),(0xe00000,0xe80000,'TVRAM'),
      (0xc00000,0xe00000,'GVRAM')]
def hwname(a):
    a &= 0xffffff
    for lo,hi,n in HW:
        if lo <= a < hi: return f'{n}+${a-lo:x}'
    return None

def main(path, out):
    x = XFile(path)
    img = bytes(x.image)
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_000)
    md.detail = True
    relocset = set(x.relocs)
    insns = {}            # addr -> (size, text, comment)
    labels = {}           # addr -> kind
    calls = {}
    todo = [x.entry]
    labels[x.entry] = 'start'
    # relocated pointers in DATA section that point into text -> code roots (fn tables)
    for r in x.relocs:
        t = x.l(r)
        if r >= x.text_end and t < x.text_end:
            todo.append(t); labels.setdefault(t, 'ptr')
    def disone(a):
        w = struct.unpack_from('>H', img, a)[0]
        if w & 0xff00 == 0xff00:
            return 2, f'dc.w\t${w:04x}', DOS.get(w, 'DOS call'), (w == 0xff00 or w == 0xff4c)
        try:
            i = next(md.disasm(img[a:a+10], a))
        except StopIteration:
            return None
        if i.id == 0 or i.mnemonic.startswith('dc.') or i.mnemonic in ('invalid','illegal'):
            return None
        return i.size, i, None, False
    seen = set()
    def run_todo():
        while todo:
          a = todo.pop()
          while 0 <= a < x.text_end and a not in seen and not (a & 1):
              r = disone(a)
              if r is None: break
              sz, i, cmt, stop = r
              seen.add(a)
              if isinstance(i, str):
                  insns[a] = (sz, i, cmt); a += sz
                  if stop: break
                  continue
              mn = i.mnemonic
              ops = i.op_str
              insns[a] = (sz, f'{mn}\t{ops}', None)
              nxt = a + sz
              tgt = None
              if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL) or mn.startswith(('b','db','j')):
                  for op in i.operands:
                      if op.type == M.M68K_OP_BR_DISP:
                          tgt = (a + 2 + op.br_disp.disp) & 0xffffffff
                      elif op.type == M.M68K_OP_MEM and op.address_mode in (M.M68K_AM_ABSOLUTE_DATA_LONG, M.M68K_AM_ABSOLUTE_DATA_SHORT):
                          tgt = op.imm if op.imm else None
                  if mn in ('jsr','jmp') and tgt is None:
                      m = re.search(r'\$([0-9a-f]+)\.l', ops)
                      if m: tgt = int(m.group(1), 16)
                      m2 = re.search(r'\$([0-9a-f]+)\(pc\)', ops)
                      if m2: tgt = int(m2.group(1),16)
              if tgt is not None and tgt < x.text_end:
                  if mn in ('jsr','bsr','bsr.s','bsr.w','bsr.b'):
                      labels[tgt] = 'fn'; calls[tgt] = calls.get(tgt,0)+1
                  else:
                      labels.setdefault(tgt, 'L')
                  todo.append(tgt)
              if mn in ('rts','rte','rtr','jmp','bra','bra.s','bra.w','bra.b','illegal') or mn.startswith('bra'):
                  break
              a = nxt
    run_todo()
    # second pass: pointers to text taken by lea/pea/move #imm in decoded code; link a6 in gaps
    for _ in range(8):
        added = 0
        for r in x.relocs:
            if r < x.text_end:
                t = x.l(r)
                if t < x.text_end and t not in seen and not (t & 1):
                    for back in (2, 4):
                        ia = r - back
                        if ia in insns and insns[ia][1].split('\t')[0] in ('lea.l','pea.l','move.l','movea.l','jsr','jmp'):
                            todo.append(t); labels.setdefault(t, 'fn' if insns[ia][1].startswith(('lea','pea','move')) else 'L'); added += 1
                            break
        for a in range(0, x.text_end - 4, 2):
            if a not in seen and img[a:a+2] == b'\x4e\x56' and (a == 0 or (a-2) not in insns or insns[a-2][1].startswith(('rts','jmp','bra'))):
                prevs = [b for b in (a-2, a-4, a-6) if b in seen]
                todo.append(a); labels.setdefault(a, 'fn'); added += 1
        run_todo()
        if not added: break
    # also relocated longs inside instructions that point into text (lea/pea/move #)
    for r in x.relocs:
        if r < x.text_end:
            t = x.l(r)
            if t < x.text_end and t in insns:
                labels.setdefault(t, 'ptr')
    def lab(a):
        k = labels.get(a)
        if k == 'start': return 'start'
        if k == 'fn': return f'fn_{a:05x}'
        return f'L{a:05x}'
    def sym(v):
        if v < x.text_end and v in labels: return lab(v)
        if x.text_end <= v < x.data_end: return f'D_{v:05x}'
        if x.data_end <= v < x.bss_end: return f'B_{v:05x}'
        return None
    with open(out, 'w') as f:
        a = 0
        while a < x.data_end:
            if a in labels:
                if labels[a] == 'fn' or labels[a]=='start': f.write('\n')
                f.write(f'{lab(a)}:\n')
            if a in insns:
                sz, t, cmt = insns[a]
                raw = img[a:a+sz].hex()
                # symbolize absolute addresses
                def rep(m):
                    v = int(m.group(1), 16)
                    s = sym(v); h = hwname(v)
                    return s or h or m.group(0)
                t2 = re.sub(r'\$([0-9a-f]{3,8})(?=\.l|\b)', rep, t)
                t2 = re.sub(r'\$([0-9a-f]+)(?=\.w)', lambda m: (hwname(int(m.group(1),16)|0xff000000) or m.group(0)), t2)
                if t.startswith(('b','db')) and not t.startswith('btst') and not t.startswith('bset') and not t.startswith('bclr') and not t.startswith('bchg'):
                    t2 = re.sub(r'\$([0-9a-f]+)$', lambda m: lab(int(m.group(1),16)) if int(m.group(1),16) in labels else m.group(0), t2)
                if t.startswith('trap\t#$f'):
                    pv = prev_moveq.get(a)
                    cmt = 'IOCS ' + (IOCS.get(pv, f'#{pv:#x}') if pv is not None else '?')
                f.write(f'  {a:05x}  {raw:<20} {t2}' + (f'\t; {cmt}' if cmt else '') + '\n')
                a += sz
            else:
                # data run
                s = a
                a += 2 if a+2 <= x.data_end else 1
                while a < x.data_end and a not in insns and a not in labels and a - s < 16:
                    a += 2
                chunk = img[s:a]
                notes = []
                for r in range(s, a):
                    if r in relocset:
                        v = x.l(r); notes.append(f'@{r:x}->{sym(v) or hex(v)}')
                f.write(f'  {s:05x}  dc.b {chunk.hex()}' + ('  ; ' + ' '.join(notes) if notes else '') + '\n')
                if a == s: a += 1
    print(len(insns), 'insns', sum(1 for v in labels.values() if v=='fn'), 'fns')

prev_moveq = {}
def precompute_moveq(path):
    pass

if __name__ == '__main__':
    import builtins
    x = XFile(sys.argv[1]); img = bytes(x.image)
    # moveq #n,d0 right before trap #15 (2 bytes earlier) -> IOCS number
    for a in range(0, x.text_end - 4, 2):
        if img[a] == 0x70 and img[a+2:a+4] == b'\x4e\x4f':
            prev_moveq[a+2] = img[a+1]
    main(sys.argv[1], sys.argv[2])
