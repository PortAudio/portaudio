#!/usr/bin/env python3
"""Test: single cffi Pa_Initialize, then ctypes for stream.
No Pa_Terminate, no double-init. Isolates cffi init vs ctypes init."""

import ctypes
import sys
import signal
import os

# Step 1: cffi Pa_Initialize (exactly what sounddevice does)
from _sounddevice import ffi as _ffi
_lib = _ffi.dlopen('/usr/local/lib/libportaudio.so.2')
err = _lib.Pa_Initialize()
print(f"cffi Pa_Initialize returned: {err}", flush=True)

# Step 2: ctypes handle to same library (already loaded)
pa = ctypes.CDLL('/usr/local/lib/libportaudio.so.2')
pa.Pa_GetVersionText.restype = ctypes.c_char_p
pa.Pa_GetErrorText.restype = ctypes.c_char_p
pa.Pa_GetErrorText.argtypes = [ctypes.c_int]
pa.Pa_OpenStream.restype = ctypes.c_int
pa.Pa_StartStream.restype = ctypes.c_int
pa.Pa_StopStream.restype = ctypes.c_int
pa.Pa_CloseStream.restype = ctypes.c_int
pa.Pa_IsStreamActive.restype = ctypes.c_int
pa.Pa_Sleep.restype = None
pa.Pa_Sleep.argtypes = [ctypes.c_long]
pa.Pa_Terminate.restype = ctypes.c_int

# Do NOT call Pa_Initialize again — use the cffi-initialized session
print(f"PA: {pa.Pa_GetVersionText().decode()}", flush=True)

class PaStreamParameters(ctypes.Structure):
    _fields_ = [
        ('device', ctypes.c_int),
        ('channelCount', ctypes.c_int),
        ('sampleFormat', ctypes.c_ulong),
        ('suggestedLatency', ctypes.c_double),
        ('hostApiSpecificStreamInfo', ctypes.c_void_p),
    ]

class PaStreamCallbackTimeInfo(ctypes.Structure):
    _fields_ = [
        ('inputBufferAdcTime', ctypes.c_double),
        ('currentTime', ctypes.c_double),
        ('outputBufferDacTime', ctypes.c_double),
    ]

PaStreamCallback = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
    ctypes.c_ulong, ctypes.POINTER(PaStreamCallbackTimeInfo),
    ctypes.c_ulong, ctypes.c_void_p,
)

CHANNELS = 2
BLOCKSIZE = 1024
count = [0]
running = [True]
signal.signal(signal.SIGINT, lambda s, f: running.__setitem__(0, False))

@PaStreamCallback
def cb(inp, outp, frames, ti, flags, ud):
    n = frames * CHANNELS
    ib = (ctypes.c_int32 * n).from_address(inp)
    ob = (ctypes.c_int32 * n).from_address(outp)
    mx = 0
    for i in range(min(n, 64)):
        v = abs(ib[i])
        if v > mx:
            mx = v
        ob[i] = ib[i]
    for i in range(64, n):
        ob[i] = ib[i]
    count[0] += 1
    if count[0] <= 20:
        sys.stderr.write(f"cb: frames={frames} max={mx}\n")
    return 0 if running[0] else 1

ip = PaStreamParameters(0, 2, 0x2, 0.0, None)
op = PaStreamParameters(0, 2, 0x2, 0.0, None)
s = ctypes.c_void_p()
err = pa.Pa_OpenStream(
    ctypes.byref(s), ctypes.byref(ip), ctypes.byref(op),
    ctypes.c_double(48000.0), ctypes.c_ulong(BLOCKSIZE),
    ctypes.c_ulong(0), cb, None,
)
if err != 0:
    print(f"Pa_OpenStream failed: {pa.Pa_GetErrorText(err).decode()}", flush=True)
    pa.Pa_Terminate()
    sys.exit(1)

err = pa.Pa_StartStream(s)
if err != 0:
    print(f"Pa_StartStream failed: {err}", flush=True)
    pa.Pa_CloseStream(s)
    pa.Pa_Terminate()
    sys.exit(1)

print("Running — Ctrl+C to stop", flush=True)
while running[0] and pa.Pa_IsStreamActive(s):
    pa.Pa_Sleep(100)

pa.Pa_StopStream(s)
pa.Pa_CloseStream(s)
pa.Pa_Terminate()
print(f"Done. {count[0]} cbs.", flush=True)
