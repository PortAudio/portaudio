#!/usr/bin/env python3
"""Test: import sounddevice + NumPy-style callback.
Tests if sounddevice's callback wrapper (NumPy arrays) causes zeros."""

import sys
import signal
import numpy as np

import sounddevice as sd

_ffi = sd._ffi
_lib = sd._lib

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

CHANNELS = 2
SAMPLESIZE = 4  # int32

@_ffi.callback('PaStreamCallback', error=_lib.paAbort)
def cb(inp, outp, frames, ti, flags, ud):
    # Mimic sounddevice's callback wrapper: create NumPy arrays from pointers
    in_buf = np.frombuffer(_ffi.buffer(
        _ffi.cast('char*', inp), frames * CHANNELS * SAMPLESIZE),
        dtype='int32').reshape(frames, CHANNELS)
    out_buf = np.frombuffer(_ffi.buffer(
        _ffi.cast('char*', outp), frames * CHANNELS * SAMPLESIZE),
        dtype='int32').reshape(frames, CHANNELS)

    mx = np.max(np.abs(in_buf))
    out_buf[:] = in_buf

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
