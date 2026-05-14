#!/usr/bin/env python3
"""Bisects what part of audio_duplex_process's pre-stream init breaks mic capture
in a multiprocessing child.

Run with no args = bare child (matches test_sd_fork_noimport.py — known to work).
Pass a level number to add cumulative init steps until capture breaks:

    LD_LIBRARY_PATH=/usr/local/lib python test_sd_fork_with_init.py 0   # bare
    LD_LIBRARY_PATH=/usr/local/lib python test_sd_fork_with_init.py 1   # +query_devices
    LD_LIBRARY_PATH=/usr/local/lib python test_sd_fork_with_init.py 2   # +AudioManager.get_device_index_by_name
    LD_LIBRARY_PATH=/usr/local/lib python test_sd_fork_with_init.py 3   # +import webrtc_audio
    LD_LIBRARY_PATH=/usr/local/lib python test_sd_fork_with_init.py 4   # +construct webrtc AudioProcessor
    LD_LIBRARY_PATH=/usr/local/lib python test_sd_fork_with_init.py 5   # +import VAD backends

Add --spawn to use spawn start method (matches the app's current setting).

Each level prints the max indata value for the first 20 callbacks. Look for the
LOWEST level number where 'max=0.0000' for the duplex.
"""
import multiprocessing
import signal
import sys
import time

USE_SPAWN = "--spawn" in sys.argv
LEVEL = 0
for a in sys.argv[1:]:
    if a.isdigit():
        LEVEL = int(a)
        break


def run_stream(level):
    import numpy as np
    import sounddevice as sd

    def _gil_safe_sleep(msec):
        time.sleep(msec / 1000.0)
    sd.sleep = _gil_safe_sleep

    sys.path.insert(0, "/home/rpidev/multiply_node/mac/python")

    if level >= 1:
        print("[child] level 1: sd.query_devices()", flush=True)
        _ = sd.query_devices()

    if level >= 2:
        print("[child] level 2: AudioManager.get_device_index_by_name", flush=True)
        from audio.audio_management import AudioManager
        am = AudioManager()
        device_name = "TAC5112: 1f000a0000.i2s-tac5x1x-hifi tac5x1x-hifi-0 (hw:0,0)"
        idx = am.get_device_index_by_name(device_name)
        print(f"[child]   device_index={idx}", flush=True)

    if level >= 3:
        print("[child] level 3: import webrtc_audio.AudioProcessor", flush=True)
        from webrtc_audio import AudioProcessor  # noqa: F401

    if level >= 4:
        print("[child] level 4: construct webrtc AudioProcessor()", flush=True)
        from webrtc_audio import AudioProcessor
        try:
            ap = AudioProcessor()
            print(f"[child]   AudioProcessor instance: {ap}", flush=True)
        except Exception as e:
            print(f"[child]   AudioProcessor() raised: {e}", flush=True)

    if level >= 5:
        print("[child] level 5: import VAD backends + WebRTCVADBackend", flush=True)
        from audio.vad_backends import WebRTCVADBackend  # noqa: F401

    count = [0]
    running = [True]
    signal.signal(signal.SIGINT, lambda s, f: running.__setitem__(0, False))
    signal.signal(signal.SIGTERM, lambda s, f: running.__setitem__(0, False))

    def callback(indata, outdata, frames, t, status):
        if status:
            sys.stderr.write(f"[child] status: {status}\n")
        mx = float(np.max(np.abs(indata)))
        outdata[:] = indata
        count[0] += 1
        if count[0] <= 20:
            sys.stderr.write(f"[child] level={level} cb#{count[0]} max={mx:.4f}\n")

    print("[child] Opening sd.Stream(device=0, channels=4, blocksize=480, float32, latency=0)", flush=True)
    stream = sd.Stream(device=0, samplerate=48000, channels=4, dtype='float32',
                       blocksize=480, latency=0, callback=callback)
    stream.start()
    print("[child] Running — Ctrl+C to stop", flush=True)
    while running[0]:
        sd.sleep(100)
    stream.stop()
    stream.close()
    print(f"[child] Done. {count[0]} cbs.", flush=True)


if __name__ == "__main__":
    method = "spawn" if USE_SPAWN else "fork"
    ctx = multiprocessing.get_context(method)
    print(f"[parent] start_method={method} LEVEL={LEVEL}", flush=True)
    p = ctx.Process(target=run_stream, args=(LEVEL,), name="sd-stream-child")
    p.start()
    print(f"[parent] Spawned child PID={p.pid}", flush=True)
    try:
        p.join()
    except KeyboardInterrupt:
        print("[parent] Ctrl+C — terminating child", flush=True)
        p.terminate()
        p.join(timeout=2)
    print(f"[parent] Child exit code: {p.exitcode}", flush=True)
