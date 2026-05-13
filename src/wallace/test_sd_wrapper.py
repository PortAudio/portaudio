#!/usr/bin/env python3
"""Test: use sounddevice's exact callback wrapper + our direct Pa_OpenStream.
If zeros → sounddevice's callback wrapper is the problem.
If works → something else in sd.Stream.__init__ is the problem."""

import sys
import signal
import numpy as np
import sounddevice as sd
from sounddevice import _wrap_callback, CallbackFlags

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

# User callback (same as test_sd_ctypes_sleep.py)
def user_callback(indata, outdata, frames, t, status):
    if status:
        sys.stderr.write(f"status: {status}\n")
    mx = np.max(np.abs(indata))
    outdata[:] = indata
    count[0] += 1
    if count[0] <= 20:
        sys.stderr.write(f"cb: frames={frames} max={mx}\n")

# sounddevice's exact wrapper for duplex+array mode
def _buffer(ptr, frames, channels, samplesize):
    return _ffi.buffer(ptr, frames * channels * samplesize)

def _array(buffer, channels, dtype):
    data = np.frombuffer(buffer, dtype=dtype)
    data.shape = -1, channels
    return data

# Mimic sd's closure variables
_channels = (CHANNELS, CHANNELS)
_dtype = ('int32', 'int32')
_samplesize = (SAMPLESIZE, SAMPLESIZE)

ffi_callback = _ffi.callback('PaStreamCallback', error=_lib.paAbort)

@ffi_callback
def callback_ptr(iptr, optr, frames, time, status, _):
    ichannels, ochannels = _channels
    idtype, odtype = _dtype
    isize, osize = _samplesize
    idata = _array(_buffer(iptr, frames, ichannels, isize), ichannels, idtype)
    odata = _array(_buffer(optr, frames, ochannels, osize), ochannels, odtype)
    return _wrap_callback(user_callback, idata, odata, frames, time, status)

# Our direct Pa_OpenStream (same as working test_sd_import.py)
ip = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
op = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
sp = _ffi.new('PaStream**')

err = _lib.Pa_OpenStream(sp, ip, op, 48000.0, 1024, 0, callback_ptr, _ffi.NULL)
if err != 0:
    print(f"Pa_OpenStream failed: {err}", flush=True)
    sys.exit(1)

err = _lib.Pa_StartStream(sp[0])
if err != 0:
    print(f"Pa_StartStream failed: {err}", flush=True)
    sys.exit(1)

stream_addr = int(_ffi.cast('uintptr_t', sp[0]))
print("Running (sd wrapper + direct open) — Ctrl+C to stop", flush=True)
while running[0] and pa.Pa_IsStreamActive(ctypes.c_void_p(stream_addr)):
    pa.Pa_Sleep(100)

pa.Pa_StopStream(ctypes.c_void_p(stream_addr))
pa.Pa_CloseStream(ctypes.c_void_p(stream_addr))
print(f"Done. {count[0]} cbs.", flush=True)
