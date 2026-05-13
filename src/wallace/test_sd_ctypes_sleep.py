#!/usr/bin/env python3
"""sounddevice stream + ctypes sleep loop.
Tests if replacing sd.sleep with ctypes Pa_Sleep fixes sounddevice."""

import sounddevice as sd
import numpy as np
import ctypes
import signal
import sys

pa = ctypes.CDLL('/usr/local/lib/libportaudio.so.2')
pa.Pa_IsStreamActive.restype = ctypes.c_int
pa.Pa_Sleep.restype = None
pa.Pa_Sleep.argtypes = [ctypes.c_long]

def callback(indata, outdata, frames, t, status):
    if status:
        sys.stderr.write(f"status: {status}\n")
    mx = np.max(np.abs(indata))
    sys.stderr.write(f"max: {mx}\n")
    outdata[:] = indata

print(f"Opening stream...", flush=True)
stream = sd.Stream(device=0, samplerate=48000, channels=2, dtype='int32',
                   blocksize=1024, latency=0, callback=callback)

print(f"Starting stream...", flush=True)
stream.start()

# Get stream pointer address for ctypes
try:
    ptr = stream._ptr
    stream_addr = int(sd._ffi.cast('uintptr_t', ptr))
    print(f"Stream ptr: {stream_addr:#x}", flush=True)
except Exception as e:
    print(f"Error getting stream ptr: {e}", flush=True)
    # Fallback: just use time.sleep
    import time
    time.sleep(3)
    stream.stop()
    stream.close()
    sys.exit(1)

running = True
signal.signal(signal.SIGINT, lambda s, f: globals().update(running=False))

print("Running (ctypes Pa_Sleep) — Ctrl+C to stop", flush=True)
try:
    while running and pa.Pa_IsStreamActive(ctypes.c_void_p(stream_addr)):
        pa.Pa_Sleep(100)
except KeyboardInterrupt:
    pass

print("Stopping...", flush=True)
stream.stop()
stream.close()
print("Done.", flush=True)
