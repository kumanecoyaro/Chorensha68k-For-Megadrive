#!/usr/bin/env python3
"""Headless Mega Drive test runner using a libretro core (e.g. Genesis Plus GX).

usage: mdrun.py CORE.so ROM.bin FRAMES OUTPREFIX [--shot N,N,...] [--input SCRIPT] [--wav]
                 [--opt key=value]   (e.g. genesis_plus_gx_ym2612=nuked (ym2612))

SCRIPT is a comma-separated list of  start-end:BUTTONS  entries, e.g.
  "60-400:R,100-500:B,400-700:L"  (U D L R A B C S = up down left right A B C start)
Screenshots are written as OUTPREFIX_NNNNN.png, audio (--wav) as OUTPREFIX.wav.
"""
import ctypes as C, sys, os, wave
from PIL import Image

RETRO_ENV_SET_PIXEL_FORMAT = 10
RETRO_ENV_GET_SYSTEM_DIRECTORY = 9
RETRO_ENV_GET_SAVE_DIRECTORY = 31
RETRO_ENV_GET_VARIABLE = 15
RETRO_ENV_GET_CAN_DUPE = 3
# libretro joypad ids
JOY = {'B': 0, 'Y': 1, 'S': 3, 'U': 4, 'D': 5, 'L': 6, 'R': 7, 'A': 8, 'X': 9}
# Genesis Plus GX default mapping: retro B -> MD B? we map MD buttons to retro ids below
MDMAP = {'A': 'Y', 'B': 'B', 'C': 'A', 'S': 'S', 'U': 'U', 'D': 'D', 'L': 'L', 'R': 'R'}

class Variable(C.Structure):
    _fields_ = [('key', C.c_char_p), ('value', C.c_char_p)]

class GameInfo(C.Structure):
    _fields_ = [('path', C.c_char_p), ('data', C.c_void_p), ('size', C.c_size_t), ('meta', C.c_char_p)]

ENV_CB = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VIDEO_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUDIO_CB = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDIOB_CB = C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_size_t)
POLL_CB = C.CFUNCTYPE(None)
STATE_CB = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)

def main():
    core, rom, frames, prefix = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    shots = set(); script = []; audio = None; opts = {}
    args = sys.argv[5:]
    while args:
        a = args.pop(0)
        if a == '--shot':
            shots = {int(v) for v in args.pop(0).split(',')}
        elif a == '--opt':                     # core option key=value
            k, v = args.pop(0).split('=', 1)
            opts[k.encode()] = C.c_char_p(v.encode())
        elif a == '--wav':
            audio = bytearray()
        elif a == '--input':
            for ent in args.pop(0).split(','):
                rng, btn = ent.split(':')
                s, e = (int(v) for v in rng.split('-'))
                script.append((s, e, btn))
    lib = C.CDLL(os.path.abspath(core))
    state = {'frame': 0, 'img': None}
    sysdir = C.c_char_p(os.path.dirname(os.path.abspath(rom)).encode())

    def env(cmd, data):
        if cmd == RETRO_ENV_SET_PIXEL_FORMAT:
            return C.cast(data, C.POINTER(C.c_int))[0] == 2   # RGB565
        if cmd in (RETRO_ENV_GET_SYSTEM_DIRECTORY, RETRO_ENV_GET_SAVE_DIRECTORY):
            C.cast(data, C.POINTER(C.c_char_p))[0] = sysdir.value
            return True
        if cmd == RETRO_ENV_GET_VARIABLE:
            var = C.cast(data, C.POINTER(Variable))[0]
            if var.key in opts:
                C.cast(data, C.POINTER(Variable))[0].value = opts[var.key].value
                return True
            return False
        if cmd == RETRO_ENV_GET_CAN_DUPE:
            C.cast(data, C.POINTER(C.c_bool))[0] = True
            return True
        return False

    def video(data, w, h, pitch):
        if not data:
            return
        buf = C.string_at(data, pitch * h)
        img = Image.frombuffer('RGB', (w, h), buf, 'raw', 'BGR;16', pitch, 1)
        state['img'] = img.copy()

    def pressed(btn):
        f = state['frame']
        return any(s <= f < e and btn in b for s, e, b in script)

    def inp(port, device, index, idn):
        if port != 0 or device != 1:
            return 0
        for md, rid in MDMAP.items():
            if JOY[rid] == idn and pressed(md):
                return 1
        return 0

    def audio_batch(data, frames):
        if audio is not None:
            audio.extend(C.string_at(data, frames * 4))
        return frames

    cbs = [ENV_CB(env), VIDEO_CB(video), AUDIO_CB(lambda l, r: None),
           AUDIOB_CB(audio_batch), POLL_CB(lambda: None), STATE_CB(inp)]
    lib.retro_set_environment(cbs[0])
    lib.retro_set_video_refresh(cbs[1])
    lib.retro_set_audio_sample(cbs[2])
    lib.retro_set_audio_sample_batch(cbs[3])
    lib.retro_set_input_poll(cbs[4])
    lib.retro_set_input_state(cbs[5])
    lib.retro_init()
    data = open(rom, 'rb').read()
    buf = C.create_string_buffer(data, len(data))
    gi = GameInfo(os.path.abspath(rom).encode(), C.cast(buf, C.c_void_p), len(data), None)
    if not lib.retro_load_game(C.byref(gi)):
        sys.exit('load failed')
    for f in range(frames):
        state['frame'] = f
        lib.retro_run()
        if f in shots and state['img'] is not None:
            state['img'].save(f'{prefix}_{f:05d}.png')
    if state['img'] is not None:
        state['img'].save(f'{prefix}_last.png')
    lib.retro_get_memory_data.restype = C.c_void_p
    lib.retro_get_memory_size.restype = C.c_size_t
    ram = lib.retro_get_memory_data(2)
    if ram:
        open(prefix + '.ram', 'wb').write(C.string_at(ram, lib.retro_get_memory_size(2)))
    vram = lib.retro_get_memory_data(3)             # RETRO_MEMORY_VIDEO_RAM (if the core has it)
    if vram:
        open(prefix + '.vram', 'wb').write(C.string_at(vram, lib.retro_get_memory_size(3)))
    if audio is not None:
        w = wave.open(prefix + '.wav', 'wb')
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
        w.writeframes(bytes(audio)); w.close()
    lib.retro_deinit()

if __name__ == '__main__':
    main()
