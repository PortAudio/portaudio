#!/usr/bin/env python3
"""Compare codec register state in the WORKING case (immediate open) vs the
BROKEN case (delayed open), and verify which produces real samples.

    LD_PRELOAD=/usr/local/lib/libportaudio.so.2 python test_codec_regs2.py

Make continuous mic noise the whole time.
"""
import os
import subprocess
import sys
import time

import numpy as np
import multiprocessing as mp

REGMAP_PATH = "/sys/kernel/debug/regmap/1-0050/registers"
TMP_DIR = "/tmp/codec_regs"
os.makedirs(TMP_DIR, exist_ok=True)


def dump(label):
    path = os.path.join(TMP_DIR, f"regs_{label}.txt")
    r = subprocess.run(["sudo", "cat", REGMAP_PATH], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  ERROR dumping {label}: {r.stderr}", file=sys.stderr)
        sys.exit(1)
    with open(path, "w") as f:
        f.write(r.stdout)
    return path


def working_child():
    import sounddevice as sd2
    mx = [0.0]; cnt = [0]
    def cb(indata, outdata, frames, t, status):
        outdata.fill(0.0)
        v = float(np.max(np.abs(indata)))
        if v > mx[0]: mx[0] = v
        cnt[0] += 1
    s = sd2.Stream(device=0, samplerate=48000, channels=4, dtype='float32',
                   blocksize=480, latency=0, callback=cb)
    s.start()
    time.sleep(0.3)
    subprocess.run(["sudo", "cat", REGMAP_PATH],
                   stdout=open(os.path.join(TMP_DIR, "regs_working_stream_open.txt"), "w"))
    time.sleep(1.2)
    s.stop()
    s.close()
    print(f"  [working] callbacks={cnt[0]} peak={mx[0]:.4f} "
          f"({'OK' if mx[0] > 0 else 'DEAD'})", flush=True)


def main():
    print("Importing sounddevice...", flush=True)
    import sounddevice as sd
    print(f"PortAudio: {sd.get_portaudio_version()}", flush=True)

    # BROKEN scenario in main process: sleep 3s, then open.
    print("\n[BROKEN] Sleeping 3s, then opening stream. Make noise!", flush=True)
    time.sleep(3.0)
    mx = [0.0]; cnt = [0]
    def cb(indata, outdata, frames, t, status):
        outdata.fill(0.0)
        v = float(np.max(np.abs(indata)))
        if v > mx[0]: mx[0] = v
        cnt[0] += 1
    s = sd.Stream(device=0, samplerate=48000, channels=4, dtype='float32',
                  blocksize=480, latency=0, callback=cb)
    s.start()
    time.sleep(0.3)
    broken_regs = dump("broken_stream_open")
    time.sleep(1.2)
    s.stop()
    s.close()
    print(f"  [broken] callbacks={cnt[0]} peak={mx[0]:.4f} "
          f"({'OK' if mx[0] > 0 else 'DEAD'})", flush=True)

    # WORKING scenario in spawn child (fresh interpreter, immediate open).
    print("\nSpawn fresh child for WORKING scenario...", flush=True)
    time.sleep(0.5)
    ctx = mp.get_context("spawn")
    p = ctx.Process(target=working_child)
    p.start()
    p.join()

    working_regs = os.path.join(TMP_DIR, "regs_working_stream_open.txt")
    print("\n=== diff BROKEN vs WORKING (registers while each stream open) ===", flush=True)
    r = subprocess.run(["diff", broken_regs, working_regs])
    if r.returncode == 0:
        print("(no diff — registers identical in both scenarios)", flush=True)


if __name__ == "__main__":
    main()
