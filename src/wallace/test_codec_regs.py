#!/usr/bin/env python3
"""Capture TAC5112 codec register state at three points:
  A) right after `import sounddevice` (Pa_Initialize done, no stream open)
  B) after 3s of idle (codec presumably settled into low-power state)
  C) while sd.Stream is open and capturing

Diffs of these three dumps should show which codec register(s) flip between
states. Specifically, A → B reveals what the codec/driver does on idle, and
B → C shows whether `sd.Stream` re-asserts the right register state.

    LD_PRELOAD=/usr/local/lib/libportaudio.so.2 python portaudio/src/wallace/test_codec_regs.py

You'll be prompted for sudo password once (regmap debugfs is root-only).
"""
import subprocess
import sys
import time
import os

REGMAP_PATH = "/sys/kernel/debug/regmap/1-0050/registers"
TMP_DIR = "/tmp/codec_regs"
os.makedirs(TMP_DIR, exist_ok=True)


def dump(label):
    path = os.path.join(TMP_DIR, f"regs_{label}.txt")
    print(f"[dump] {label} → {path}", flush=True)
    # Read via sudo cat and write into our tmp file (avoid sudo tee for simplicity).
    r = subprocess.run(["sudo", "cat", REGMAP_PATH], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  ERROR: sudo cat failed (rc={r.returncode}): {r.stderr}", file=sys.stderr)
        sys.exit(1)
    with open(path, "w") as f:
        f.write(r.stdout)
    return path


print("Importing sounddevice (calls Pa_Initialize)...", flush=True)
import sounddevice as sd
print(f"PortAudio: {sd.get_portaudio_version()}", flush=True)

a_path = dump("A_after_import")

print("Sleeping 3s with NO stream open...", flush=True)
time.sleep(3.0)

b_path = dump("B_after_3s_idle")

import numpy as np

def cb(indata, outdata, frames, t, status):
    outdata.fill(0.0)

print("Opening sd.Stream...", flush=True)
stream = sd.Stream(device=0, samplerate=48000, channels=4, dtype='float32',
                   blocksize=480, latency=0, callback=cb)
stream.start()
time.sleep(0.5)
c_path = dump("C_stream_open")
stream.stop()
stream.close()

print("\n=== diff A → B (changes during idle, no stream open) ===", flush=True)
subprocess.run(["diff", a_path, b_path])

print("\n=== diff B → C (changes when sd.Stream opens after idle) ===", flush=True)
subprocess.run(["diff", b_path, c_path])

print("\n=== diff A → C (overall: post-import vs streaming) ===", flush=True)
subprocess.run(["diff", a_path, c_path])

print(f"\nFull dumps saved in {TMP_DIR}/", flush=True)
