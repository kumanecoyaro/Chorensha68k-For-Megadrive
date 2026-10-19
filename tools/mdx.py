"""MDX (MXDRV / MCDRV sequence data, X68000 YM2151) parser and tick interpreter."""
import struct

class MDX:
    def __init__(self, data):
        p = data.index(b'\r\n\x1a')
        self.title = data[:p].decode('cp932', 'replace')
        q = data.index(b'\0', p + 3)
        self.pdx = data[p + 3:q].decode('cp932', 'replace')
        self.base = q + 1
        b = data[self.base:]
        self.body = b
        voff = struct.unpack_from('>H', b, 0)[0]
        first = struct.unpack_from('>H', b, 2)[0]
        ntr = (first - 2) // 2
        self.tracks = [struct.unpack_from('>H', b, 2 + 2 * i)[0] for i in range(ntr)]
        self.voices = {}
        o = voff
        while o + 27 <= len(b):
            v = b[o:o + 27]
            self.voices[v[0]] = Voice(v)
            o += 27

class Voice:
    """27-byte MDX voice; operator order M1, M2, C1, C2 (OPM register order)."""
    def __init__(self, v):
        self.no = v[0]
        self.fb = (v[1] >> 3) & 7
        self.con = v[1] & 7
        self.mask = v[2] & 15
        self.dt1mul = list(v[3:7])
        self.tl = list(v[7:11])
        self.ksar = list(v[11:15])
        self.amed1r = list(v[15:19])
        self.dt2d2r = list(v[19:23])
        self.d1lrr = list(v[23:27])

CARRIERS = {0: (3,), 1: (3,), 2: (3,), 3: (3,), 4: (2, 3), 5: (1, 2, 3), 6: (1, 2, 3), 7: (0, 1, 2, 3)}
VOL_TABLE = [0x2A, 0x28, 0x25, 0x22, 0x20, 0x1D, 0x1A, 0x18, 0x15, 0x12, 0x10, 0x0D, 0x0A, 0x08, 0x05, 0x02]

class Track:
    """Interprets one MDX track, producing events per tick."""
    def __init__(self, mdx, idx):
        self.m = mdx; self.idx = idx
        self.pos = mdx.tracks[idx]
        self.b = mdx.body
        self.wait = 0
        self.ended = False
        self.loop_tick = None        # tick at which the loop target was first reached
        self.loop_pos = None
        self.voice = None
        self.vol = 8; self.atvol = None      # v / @v
        self.pan = 3
        self.q = 8
        self.detune = 0
        self.porta = 0
        self.legato = False          # $F7: the next note is not keyed off at its end
        self.tie = False             # previous note was tied
        self.keyon_delay = 0
        self.mp = None; self.ma = None
        self.mp_on = False; self.ma_on = False
        self.noise = None            # $ED value (channel H noise)
        self.stack = []
        self.events = []             # filled per step: ('note', n, len, gate) etc.
        self.visited = {}

    def u8(self):
        v = self.b[self.pos]; self.pos += 1; return v

    def s16(self):
        v = struct.unpack_from('>h', self.b, self.pos)[0]; self.pos += 2; return v

    def u16(self):
        v = struct.unpack_from('>H', self.b, self.pos)[0]; self.pos += 2; return v

    def step(self, tick, tempo_cb):
        """Run commands until a note/rest; return list of events for this tick."""
        ev = []
        if self.ended:
            return ev
        if self.wait > 0:
            self.wait -= 1
            return ev
        guard = 0
        while True:
            guard += 1
            if guard > 10000:
                self.ended = True
                return ev
            if self.pos not in self.visited:
                self.visited[self.pos] = tick
            c = self.u8()
            if c <= 0x7F:                                   # rest
                ev.append(('rest', c + 1))
                self.wait = c
                return ev
            if c <= 0xDF:                                   # note
                ln = self.u8() + 1
                if self.legato or self.q == 8:
                    gate = 0                                 # no gate key-off
                elif self.q < 8:
                    gate = max(1, ln * self.q // 8)
                else:
                    gate = max(1, ln - (256 - self.q))
                # e[4]: this note is tied to the previous one (no key-on)
                ev.append(('note', c - 0x80, ln, gate, self.tie))
                self.tie = self.legato
                self.legato = False
                self.wait = ln - 1
                return ev
            if c == 0xFF: tempo_cb(self.u8())
            elif c == 0xFE: r = self.u8(); v = self.u8(); ev.append(('opm', r, v))
            elif c == 0xFD: self.voice = self.u8(); ev.append(('voice', self.voice))
            elif c == 0xFC: self.pan = self.u8() & 3; ev.append(('pan', self.pan))
            elif c == 0xFB:
                v = self.u8()
                if v & 0x80: self.atvol = v & 0x7F
                else: self.vol = v & 15; self.atvol = None
                ev.append(('vol',))
            elif c == 0xFA:                                  # ( volume down
                if self.atvol is not None: self.atvol = min(127, self.atvol + 1)   # @v = attenuation
                else: self.vol = max(0, self.vol - 1)
                ev.append(('vol',))
            elif c == 0xF9:                                  # ) volume up
                if self.atvol is not None: self.atvol = max(0, self.atvol - 1)
                else: self.vol = min(15, self.vol + 1)
                ev.append(('vol',))
            elif c == 0xF8: self.q = self.u8()
            elif c == 0xF7: self.legato = True
            elif c == 0xF6:
                cnt = self.u8(); self.u8()
                self.stack.append([cnt, self.pos])
            elif c == 0xF5:
                off = self.s16()
                if self.stack:
                    top = self.stack[-1]
                    top[0] -= 1
                    if top[0] > 0:
                        self.pos = top[1]
                    else:
                        self.stack.pop()
            elif c == 0xF4:
                off = self.s16()
                if self.stack and self.stack[-1][0] == 1:
                    self.stack.pop()
                    self.pos += off + 2       # target is the $F5 operand; continue after it
            elif c == 0xF3: self.detune = self.s16(); ev.append(('pitch',))
            elif c == 0xF2: self.porta = self.s16()
            elif c == 0xF1:
                hi = self.u8()
                if hi == 0:
                    self.ended = True
                    return ev
                off = struct.unpack('>h', bytes([hi, self.u8()]))[0]
                self.pos += off
                if self.loop_pos is None:
                    self.loop_pos = self.pos
                    self.loop_tick = self.visited.get(self.pos, 0)
                    self.loop_end_tick = tick
                    ev.append(('looped',))
            elif c == 0xF0: self.keyon_delay = self.u8()
            elif c == 0xEF: self.u8()                        # sync send (ignored)
            elif c == 0xEE: pass                             # sync wait (ignored)
            elif c == 0xED: self.noise = self.u8(); ev.append(('noise', self.noise))
            elif c == 0xEC:
                w = self.u8()
                if w == 0x80: self.mp_on = False
                elif w == 0x81: self.mp_on = self.mp is not None
                else: self.mp = (w, self.u16(), self.s16()); self.mp_on = True
            elif c == 0xEB:
                w = self.u8()
                if w == 0x80: self.ma_on = False
                elif w == 0x81: self.ma_on = self.ma is not None
                else: self.ma = (w, self.u16(), self.s16()); self.ma_on = True
            elif c == 0xEA:                                  # OPM hardware LFO (ignored)
                w = self.u8()
                if w not in (0x80, 0x81):
                    self.pos += 4
            elif c == 0xE9: self.u8()                        # LFO delay (ignored)
            elif c == 0xE8: pass                             # PCM8 mode
            elif c == 0xE7:                                  # fade out
                if self.u8() == 1: self.u8()
            else:
                raise ValueError(f'track {self.idx}: unknown MDX command {c:#x} at {self.pos - 1:#x}')
