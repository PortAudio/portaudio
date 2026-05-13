#!/usr/bin/env python3
"""Test: sd.Stream with float32 dtype (same as multiply_node app)."""

import sys
import time
import signal
import numpy as np
import sounddevice as sd

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
        sys.stderr.write(f"cb: frames={frames} max={mx:.6f} dtype={indata.dtype}\n")

# Match multiply_node params: float32, 480 frames, 48kHz
stream = sd.Stream(device=0, samplerate=48000, channels=2, dtype=np.float32,
                   blocksize=480, latency=0.05, callback=callback)
stream.start()

print("Running (float32, bs=480, lat=0.05) — Ctrl+C to stop", flush=True)
while running[0]:
    time.sleep(0.1)

stream.stop()
stream.close()
print(f"Done. {count[0]} cbs.", flush=True)
