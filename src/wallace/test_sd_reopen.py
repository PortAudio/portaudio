#!/usr/bin/env python3
"""Tests whether opening/closing/re-opening sd.Stream on hw:0,0 breaks capture.

Hypothesis: the patched ALSA backend on this codec leaves it in a state where
the FIRST sd.Stream captures real samples but every subsequent open returns
silence — even when each stream is properly stopped+closed before the next.

Run:
    LD_PRELOAD=/usr/local/lib/libportaudio.so.2 python portaudio/src/wallace/test_sd_reopen.py
"""
import sys
import time

import numpy as np
import sounddevice as sd


def open_capture_check(label, duration_s=0.8):
    cnt = [0]; mx = [0.0]
    def cb(indata, outdata, frames, t, status):
        outdata.fill(0.0)
        v = float(np.max(np.abs(indata)))
        if v > mx[0]:
            mx[0] = v
        cnt[0] += 1
    s = sd.Stream(device=0, samplerate=48000, channels=4,
                  blocksize=480, callback=cb, latency=0, dtype='float32')
    s.start()
    time.sleep(duration_s)
    s.stop()
    s.close()
    print(f"[{label}] callbacks={cnt[0]} peak={mx[0]:.4f} "
          f"({'OK' if mx[0] > 0 else 'DEAD'})", flush=True)
    return mx[0] > 0


print(f"PortAudio: {sd.get_portaudio_version()}", flush=True)
print(f"sd module: {sd.__file__}", flush=True)
print("Tap/talk into the mic during each window.", flush=True)

for i in range(1, 5):
    open_capture_check(f"open#{i}")
    time.sleep(0.5)
