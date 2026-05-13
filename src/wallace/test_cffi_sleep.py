#!/usr/bin/env python3
"""Test: cffi init + ctypes OpenStream/callback + cffi Pa_Sleep loop.
If zeros → cffi Pa_Sleep (GIL held during sleep) is the problem.
If works → cffi Pa_OpenStream or Pa_StartStream is the problem."""

import ctypes
import sys
import signal

# cffi init (same as sounddevice)
from _sounddevice import ffi as _ffi
_lib = _ffi.dlopen('/usr/local/lib/libportaudio.so.2')
_lib.Pa_Initialize()

# ctypes handle
pa = ctypes.CDLL('/usr/local/lib/libportaudio.so.2')
pa.Pa_GetVersionText.restype = ctypes.c_char_p
pa.Pa_OpenStream.restype = ctypes.c_int
pa.Pa_StartStream.restype = ctypes.c_int
pa.Pa_StopStream.restype = ctypes.c_int
pa.Pa_CloseStream.restype = ctypes.c_int
pa.Pa_Terminate.restype = ctypes.c_int

class PSP(ctypes.Structure):
    _fields_ = [('device',ctypes.c_int),('channelCount',ctypes.c_int),
                ('sampleFormat',ctypes.c_ulong),('suggestedLatency',ctypes.c_double),
                ('hostApiSpecificStreamInfo',ctypes.c_void_p)]
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

# ctypes Pa_OpenStream and Pa_StartStream
ip = PSP(0, 2, 0x2, 0.0, None)
op = PSP(0, 2, 0x2, 0.0, None)
s = ctypes.c_void_p()
pa.Pa_OpenStream(ctypes.byref(s), ctypes.byref(ip), ctypes.byref(op),
                 ctypes.c_double(48000.0), ctypes.c_ulong(1024),
                 ctypes.c_ulong(0), cb, None)
pa.Pa_StartStream(s)
print("Running (cffi Pa_Sleep loop) — Ctrl+C to stop", flush=True)

# === USE CFFI Pa_Sleep (holds GIL) instead of ctypes ===
stream_handle = _ffi.cast('PaStream*', int(s.value))
while running[0] and _lib.Pa_IsStreamActive(stream_handle):
    _lib.Pa_Sleep(100)

pa.Pa_StopStream(s)
pa.Pa_CloseStream(s)
pa.Pa_Terminate()
print(f"Done. {count[0]} cbs.", flush=True)
