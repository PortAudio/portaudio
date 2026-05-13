#!/usr/bin/env python3
"""Test: cffi callback + cffi OpenStream/StartStream + ctypes sleep.
If zeros → cffi callback mechanism is the root cause."""

import sys
import signal

from _sounddevice import ffi as _ffi
_lib = _ffi.dlopen('/usr/local/lib/libportaudio.so.2')
_lib.Pa_Initialize()

import ctypes
pa = ctypes.CDLL('/usr/local/lib/libportaudio.so.2')
pa.Pa_IsStreamActive.restype = ctypes.c_int
pa.Pa_Sleep.restype = None
pa.Pa_Sleep.argtypes = [ctypes.c_long]
pa.Pa_StopStream.restype = ctypes.c_int
pa.Pa_CloseStream.restype = ctypes.c_int
pa.Pa_Terminate.restype = ctypes.c_int

count = [0]
running = [True]
signal.signal(signal.SIGINT, lambda s,f: running.__setitem__(0, False))

# === CFFI callback (instead of ctypes) ===
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

# Build cffi params
ip = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
op = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
sp = _ffi.new('PaStream**')

# cffi Pa_OpenStream with cffi callback
err = _lib.Pa_OpenStream(sp, ip, op, 48000.0, 1024, 0, cb, _ffi.NULL)
if err != 0:
    print(f"Pa_OpenStream failed: {err}", flush=True)
    sys.exit(1)
print("cffi Pa_OpenStream: OK", flush=True)

# cffi Pa_StartStream
err = _lib.Pa_StartStream(sp[0])
if err != 0:
    print(f"Pa_StartStream failed: {err}", flush=True)
    sys.exit(1)

# ctypes Pa_Sleep loop (releases GIL)
stream_addr = int(_ffi.cast('uintptr_t', sp[0]))
ct_stream = ctypes.c_void_p(stream_addr)
print("Running (cffi callback, ctypes sleep) — Ctrl+C to stop", flush=True)
while running[0] and pa.Pa_IsStreamActive(ct_stream):
    pa.Pa_Sleep(100)

pa.Pa_StopStream(ct_stream)
pa.Pa_CloseStream(ct_stream)
pa.Pa_Terminate()
print(f"Done. {count[0]} cbs.", flush=True)
