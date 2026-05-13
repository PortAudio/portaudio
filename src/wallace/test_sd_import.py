#!/usr/bin/env python3
"""Test: import sounddevice (full import with Pa_Initialize),
then use our known-working cffi approach for the stream.
If zeros → sounddevice's import corrupts something.
If works → sounddevice's Stream class setup is the issue."""

import sys
import signal

# Full sounddevice import (does Pa_Initialize, registers atexit, etc.)
import sounddevice as sd

_ffi = sd._ffi
_lib = sd._lib
# NOTE: Pa_Initialize already done by sounddevice — skip our own

import ctypes
pa = ctypes.CDLL('/usr/local/lib/libportaudio.so.2')
pa.Pa_IsStreamActive.restype = ctypes.c_int
pa.Pa_Sleep.restype = None
pa.Pa_Sleep.argtypes = [ctypes.c_long]
pa.Pa_StopStream.restype = ctypes.c_int
pa.Pa_CloseStream.restype = ctypes.c_int

count = [0]
running = [True]
signal.signal(signal.SIGINT, lambda s, f: running.__setitem__(0, False))

@_ffi.callback('PaStreamCallback', error=_lib.paAbort)
def cb(inp, outp, frames, ti, flags, ud):
    n = frames * 2
    in_buf = _ffi.cast('int32_t*', inp)
    out_buf = _ffi.cast('int32_t*', outp)
    mx = 0
    for i in range(min(n, 64)):
        v = in_buf[i] if in_buf[i] >= 0 else -in_buf[i]
        if v > mx: mx = v
        out_buf[i] = in_buf[i]
    for i in range(64, n):
        out_buf[i] = in_buf[i]
    count[0] += 1
    if count[0] <= 20: sys.stderr.write(f"cb: frames={frames} max={mx}\n")
    return _lib.paContinue if running[0] else _lib.paComplete

ip = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
op = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
sp = _ffi.new('PaStream**')

err = _lib.Pa_OpenStream(sp, ip, op, 48000.0, 1024, 0, cb, _ffi.NULL)
if err != 0:
    print(f"Pa_OpenStream failed: {err}", flush=True)
    sys.exit(1)

err = _lib.Pa_StartStream(sp[0])
if err != 0:
    print(f"Pa_StartStream failed: {err}", flush=True)
    sys.exit(1)

stream_addr = int(_ffi.cast('uintptr_t', sp[0]))
print("Running — Ctrl+C to stop", flush=True)
while running[0] and pa.Pa_IsStreamActive(ctypes.c_void_p(stream_addr)):
    pa.Pa_Sleep(100)

pa.Pa_StopStream(ctypes.c_void_p(stream_addr))
pa.Pa_CloseStream(ctypes.c_void_p(stream_addr))
print(f"Done. {count[0]} cbs.", flush=True)
