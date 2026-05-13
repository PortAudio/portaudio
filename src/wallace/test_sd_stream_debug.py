#!/usr/bin/env python3
"""Debug sd.Stream: dump all internal state, then run with ctypes sleep."""

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

count = [0]

def callback(indata, outdata, frames, t, status):
    if status:
        sys.stderr.write(f"status: {status}\n")
    mx = np.max(np.abs(indata))
    outdata[:] = indata
    count[0] += 1
    if count[0] <= 20:
        sys.stderr.write(f"cb: frames={frames} max={mx}\n")

# Create stream with EXPLICIT params matching our working test
stream = sd.Stream(device=0, samplerate=48000, channels=2, dtype='int32',
                   blocksize=1024, latency=0, callback=callback)

# Dump internal state
print(f"stream._device = {stream._device}", flush=True)
print(f"stream._channels = {stream._channels}", flush=True)
print(f"stream._dtype = {stream._dtype}", flush=True)
print(f"stream._samplesize = {stream._samplesize}", flush=True)
print(f"stream._blocksize = {stream._blocksize}", flush=True)
print(f"stream.samplerate = {stream.samplerate}", flush=True)
print(f"stream._latency = {stream._latency}", flush=True)

# Check stream info
info = _lib.Pa_GetStreamInfo(stream._ptr)
print(f"streamInfo.sampleRate = {info.sampleRate}", flush=True)
print(f"streamInfo.inputLatency = {info.inputLatency}", flush=True)
print(f"streamInfo.outputLatency = {info.outputLatency}", flush=True)

# Also create our direct stream to compare info
ip = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
op = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
sp = _ffi.new('PaStream**')
err = _lib.Pa_OpenStream(sp, ip, op, 48000.0, 1024, 0, _ffi.NULL, _ffi.NULL)
if err == 0:
    info2 = _lib.Pa_GetStreamInfo(sp[0])
    print(f"\ndirect streamInfo.sampleRate = {info2.sampleRate}", flush=True)
    print(f"direct streamInfo.inputLatency = {info2.inputLatency}", flush=True)
    print(f"direct streamInfo.outputLatency = {info2.outputLatency}", flush=True)
    _lib.Pa_CloseStream(sp[0])
else:
    print(f"direct Pa_OpenStream failed: {err}", flush=True)

# Now start and run with ctypes sleep
stream.start()

stream_addr = int(_ffi.cast('uintptr_t', stream._ptr))
running = True
signal.signal(signal.SIGINT, lambda s, f: globals().update(running=False))

print("\nRunning (sd.Stream + ctypes sleep) — Ctrl+C to stop", flush=True)
try:
    while running and pa.Pa_IsStreamActive(ctypes.c_void_p(stream_addr)):
        pa.Pa_Sleep(100)
except KeyboardInterrupt:
    pass

stream.stop()
stream.close()
print(f"Done. {count[0]} cbs.", flush=True)
