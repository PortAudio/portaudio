#!/usr/bin/env python3
"""Confirms codec auto-suspends a few seconds after Pa_Initialize.

import sounddevice triggers Pa_Initialize. Wait, then open stream — codec
should return silent samples if suspend hypothesis holds.

    LD_PRELOAD=/usr/local/lib/libportaudio.so.2 python test_sd_suspend.py
"""
import sys
import time

import numpy as np

print("Importing sounddevice (will call Pa_Initialize)...", flush=True)
import sounddevice as sd
print(f"PortAudio: {sd.get_portaudio_version()}", flush=True)

print("Sleeping 5s with NO stream open. Make noise into mic anyway.", flush=True)
time.sleep(5.0)

cnt = [0]; mx = [0.0]
def cb(indata, outdata, frames, t, status):
    outdata.fill(0.0)
    v = float(np.max(np.abs(indata)))
    if v > mx[0]: mx[0] = v
    cnt[0] += 1

print("Opening sd.Stream now — make noise for 3s", flush=True)
with sd.Stream(device=0, samplerate=48000, channels=4, dtype='float32',
               blocksize=480, latency=0, callback=cb):
    time.sleep(3.0)
print(f"RESULT: callbacks={cnt[0]} peak={mx[0]:.4f} "
      f"({'OK' if mx[0] > 0 else 'DEAD (codec suspended)'})", flush=True)
