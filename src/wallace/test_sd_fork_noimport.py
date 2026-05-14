#!/usr/bin/env python3
"""Step 1b: same as test_sd_fork.py but the PARENT never imports sounddevice.

If test_sd_fork.py fails (both fork and spawn) but THIS works, the issue is
specifically "parent process has libportaudio loaded + child opens hw:0,0"
— i.e. two libportaudio loads in different processes simultaneously breaks
ALSA capture on this codec.

If THIS also produces zeros, the issue is broader than parent-side sd state.

Run:
    LD_LIBRARY_PATH=/usr/local/lib python portaudio/src/wallace/test_sd_fork_noimport.py
    LD_LIBRARY_PATH=/usr/local/lib python portaudio/src/wallace/test_sd_fork_noimport.py --spawn
"""

import multiprocessing
import signal
import sys
import time

# NOTE: deliberately NOT importing sounddevice here in the parent.

USE_SPAWN = "--spawn" in sys.argv


def run_stream():
    import numpy as np
    import sounddevice as sd  # imported only in CHILD

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

    print(f"[child] sd module: {sd.__file__}", flush=True)
    print(f"[child] PortAudio: {sd.get_portaudio_version()}", flush=True)
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
        print("[parent] Using SPAWN start method (parent never imports sd)", flush=True)
    else:
        ctx = multiprocessing.get_context("fork")
        print("[parent] Using FORK start method (parent never imports sd)", flush=True)

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
