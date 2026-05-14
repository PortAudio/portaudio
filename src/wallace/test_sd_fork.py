#!/usr/bin/env python3
"""Step 1 of fork-bug diagnosis: same stream as test_sd_final.py, but opened
inside a multiprocessing.Process child (default 'fork' on Linux).

Sounddevice is imported at module scope in the parent, mirroring how app.py
imports audio.process_manager -> audio.audio_duplex -> sounddevice before any
child is spawned. If libportaudio cannot survive fork on this patched build,
the child's indata will be silent even though the parent-process variant
(test_sd_final.py) works.

Run:
    LD_LIBRARY_PATH=/usr/local/lib python portaudio/src/wallace/test_sd_fork.py

Flip USE_SPAWN=True (or pass --spawn) to switch to the spawn start method and
confirm whether avoiding fork restores capture.
"""

import multiprocessing
import signal
import sys
import time

import numpy as np
import sounddevice as sd  # imported in PARENT — matches app.py's import chain

USE_SPAWN = "--spawn" in sys.argv


def run_stream():
    # Monkey-patch sd.sleep here too — child needs its own patch since it's
    # a fresh interpreter under spawn (and harmless under fork).
    def _gil_safe_sleep(msec):
        time.sleep(msec / 1000.0)
    sd.sleep = _gil_safe_sleep

    count = [0]
    running = [True]
    signal.signal(signal.SIGINT, lambda s, f: running.__setitem__(0, False))
    signal.signal(signal.SIGTERM, lambda s, f: running.__setitem__(0, False))

    def callback(indata, outdata, frames, t, status):
        if status:
            sys.stderr.write(f"[child] status: {status}\n")
        mx = np.max(np.abs(indata))
        per_ch = [float(np.max(np.abs(indata[:, c]))) for c in range(indata.shape[1])]
        outdata[:] = indata
        count[0] += 1
        if count[0] <= 20:
            sys.stderr.write(f"[child] cb#{count[0]} frames={frames} max={mx:.4f} per_ch={per_ch}\n")

    print("[child] Opening sd.Stream...", flush=True)
    stream = sd.Stream(device=0, samplerate=48000, channels=4, dtype='float32',
                       blocksize=480, latency=0, callback=callback)
    print("[child] Starting stream...", flush=True)
    stream.start()
    print("[child] Running — Ctrl+C in parent to stop", flush=True)
    while running[0]:
        sd.sleep(100)
    stream.stop()
    stream.close()
    print(f"[child] Done. {count[0]} cbs.", flush=True)


if __name__ == "__main__":
    if USE_SPAWN:
        ctx = multiprocessing.get_context("spawn")
        print("[parent] Using SPAWN start method", flush=True)
    else:
        ctx = multiprocessing.get_context("fork")
        print("[parent] Using FORK start method (default on Linux)", flush=True)

    print(f"[parent] sd module: {sd.__file__}", flush=True)
    print(f"[parent] PortAudio version: {sd.get_portaudio_version()}", flush=True)

    p = ctx.Process(target=run_stream, name="sd-stream-child")
    p.start()
    print(f"[parent] Spawned child PID={p.pid}", flush=True)
    try:
        p.join()
    except KeyboardInterrupt:
        print("[parent] Ctrl+C — terminating child", flush=True)
        p.terminate()
        p.join(timeout=2)
    print(f"[parent] Child exit code: {p.exitcode}", flush=True)
