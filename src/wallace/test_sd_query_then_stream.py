#!/usr/bin/env python3
"""Reproduces the audio_duplex.py pre-stream pattern: call sd.query_devices()
before opening sd.Stream(device=0, ...). If this loses mic capture while
test_sd_final.py keeps it, the device enumeration is poisoning hw:0,0.

Run:
    LD_LIBRARY_PATH=/usr/local/lib python portaudio/src/wallace/test_sd_query_then_stream.py
"""
import signal
import sys
import time

import numpy as np
import sounddevice as sd


def _gil_safe_sleep(msec):
    time.sleep(msec / 1000.0)
sd.sleep = _gil_safe_sleep

count = [0]
running = [True]
signal.signal(signal.SIGINT, lambda s, f: running.__setitem__(0, False))


def callback(indata, outdata, frames, t, status):
    if status:
        sys.stderr.write(f"status: {status}\n")
    mx = float(np.max(np.abs(indata)))
    outdata[:] = indata
    count[0] += 1
    if count[0] <= 20:
        sys.stderr.write(f"cb#{count[0]} frames={frames} max={mx}\n")


print("Calling sd.query_devices() BEFORE opening stream...", flush=True)
devs = sd.query_devices()
print(f"Found {len(devs)} devices", flush=True)
for i, d in enumerate(devs):
    print(f"  [{i}] {d['name']}  in={d['max_input_channels']}  out={d['max_output_channels']}", flush=True)

print("Now opening sd.Stream(device=0, ...)...", flush=True)
stream = sd.Stream(device=0, samplerate=48000, channels=4, dtype='float32',
                   blocksize=480, latency=0, callback=callback)
stream.start()
print("Running — Ctrl+C to stop", flush=True)
while running[0]:
    sd.sleep(100)
stream.stop()
stream.close()
print(f"Done. {count[0]} cbs.", flush=True)
