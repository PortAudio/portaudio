#!/usr/bin/env python3
"""Test: cffi Pa_OpenStream + ctypes everything else.
If zeros → cffi Pa_OpenStream is the problem.
If works → cffi Pa_StartStream is the problem."""

import ctypes
import sys
import signal

from _sounddevice import ffi as _ffi
_lib = _ffi.dlopen('/usr/local/lib/libportaudio.so.2')
_lib.Pa_Initialize()

pa = ctypes.CDLL('/usr/local/lib/libportaudio.so.2')
pa.Pa_StartStream.restype = ctypes.c_int
pa.Pa_StopStream.restype = ctypes.c_int
pa.Pa_CloseStream.restype = ctypes.c_int
pa.Pa_IsStreamActive.restype = ctypes.c_int
pa.Pa_Sleep.restype = None
pa.Pa_Sleep.argtypes = [ctypes.c_long]
pa.Pa_Terminate.restype = ctypes.c_int

class TI(ctypes.Structure):
    _fields_ = [('a',ctypes.c_double),('b',ctypes.c_double),('c',ctypes.c_double)]
CB = ctypes.CFUNCTYPE(ctypes.c_int,ctypes.c_void_p,ctypes.c_void_p,
                      ctypes.c_ulong,ctypes.POINTER(TI),ctypes.c_ulong,ctypes.c_void_p)

count = [0]
running = [True]
signal.signal(signal.SIGINT, lambda s,f: running.__setitem__(0, False))

@CB
def cb(inp, outp, frames, ti, flags, ud):
    n = frames * 2
    ib = (ctypes.c_int32 * n).from_address(inp)
    ob = (ctypes.c_int32 * n).from_address(outp)
    mx = 0
    for i in range(min(n, 64)):
        v = abs(ib[i])
        if v > mx: mx = v
        ob[i] = ib[i]
    for i in range(64, n): ob[i] = ib[i]
    count[0] += 1
    if count[0] <= 20: sys.stderr.write(f"cb: frames={frames} max={mx}\n")
    return 0 if running[0] else 1

# Build cffi params
ip = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
op = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
sp = _ffi.new('PaStream**')

# Convert ctypes callback to cffi function pointer
cb_addr = ctypes.cast(cb, ctypes.c_void_p).value
cffi_cb = _ffi.cast('PaStreamCallback', cb_addr)

# === CFFI Pa_OpenStream ===
err = _lib.Pa_OpenStream(sp, ip, op, 48000.0, 1024, 0, cffi_cb, _ffi.NULL)
if err != 0:
    print(f"Pa_OpenStream failed: {err}", flush=True)
    sys.exit(1)
print("cffi Pa_OpenStream: OK", flush=True)

# Get stream pointer for ctypes
stream_addr = int(_ffi.cast('uintptr_t', sp[0]))
ct_stream = ctypes.c_void_p(stream_addr)

# ctypes Pa_StartStream + Pa_Sleep loop
err = pa.Pa_StartStream(ct_stream)
if err != 0:
    print(f"Pa_StartStream failed: {err}", flush=True)
    sys.exit(1)

print("Running — Ctrl+C to stop", flush=True)
while running[0] and pa.Pa_IsStreamActive(ct_stream):
    pa.Pa_Sleep(100)

pa.Pa_StopStream(ct_stream)
pa.Pa_CloseStream(ct_stream)
pa.Pa_Terminate()
print(f"Done. {count[0]} cbs.", flush=True)
