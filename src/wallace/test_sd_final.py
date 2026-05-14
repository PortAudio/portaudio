#!/usr/bin/env python3
"""Final test: sounddevice used normally with monkey-patched sleep.
Patches sd.sleep to use time.sleep instead of cffi Pa_Sleep (GIL fix)."""

import sys
import time
import signal
import numpy as np
import sounddevice as sd

# Monkey-patch sd.sleep to release GIL during sleep
_original_sleep = sd.sleep
def _gil_safe_sleep(msec):
    time.sleep(msec / 1000.0)
sd.sleep = _gil_safe_sleep

count = [0]
running = [True]
signal.signal(signal.SIGINT, lambda s, f: running.__setitem__(0, False))

def callback(indata, outdata, frames, t, status):
    if status:
        sys.stderr.write(f"status: {status}\n")
    mx = np.max(np.abs(indata))
    outdata[:] = indata
    count[0] += 1
    if count[0] <= 20:
        sys.stderr.write(f"cb: frames={frames} max={mx}\n")

print("Opening sd.Stream...", flush=True)
stream = sd.Stream(device=0, samplerate=48000, channels=4, dtype='float32',
                   blocksize=480, latency=0, callback=callback)

print("Starting...", flush=True)
stream.start()

print("Running (sd.Stream + patched sleep) — Ctrl+C to stop", flush=True)
while running[0]:
    sd.sleep(100)

stream.stop()
stream.close()
print(f"Done. {count[0]} cbs.", flush=True)
