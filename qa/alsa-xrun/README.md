# ALSA non-MMAP capture recovery reproducer

This opt-in Linux test uses sounddevice's callback API, the supplied PortAudio
build, and an ALSA null/plug PCM. It needs no microphone, `/dev/snd`, privileges,
or running audio server. It is separate from the normal cross-platform build.

The fixture rejects MMAP access, then reports one XRUN after five callbacks.
It leaves `snd_pcm_recover()` and `snd_pcm_start()` real, except for the requested
fault. Capture is restarted three times per successful case. All fault controls
run on the callback thread; reset runs after close. Concurrent streams are not
supported. The null PCM samples are never inspected or saved.

## Run the baseline comparison

From the candidate checkout (with Docker available):

```sh
baseline_dir=$(mktemp -d)
output_dir=$(mktemp -d)
git archive 7c45fed9322bd86b5dd0da4547b15d9ac3b9b0ac | tar -x -C "$baseline_dir"
docker build -t pa-alsa-xrun-test qa/alsa-xrun
docker run --rm \
    -v "$baseline_dir:/baseline:ro" \
    -v "$PWD:/candidate:ro" \
    -v "$output_dir:/output" \
    pa-alsa-xrun-test python /candidate/qa/alsa-xrun/run.py \
    --baseline /baseline --candidate /candidate --output /output/results
```

Use a fresh output directory for each run. The script builds both trees with
ALSA enabled and JACK disabled, using dynamic linking so LD_PRELOAD can intercept
ALSA calls. Build logs, per-case logs and `results.json` remain in the output
directory. A timeout, build failure or unexpected result makes the command fail.
The runner checks callback counts and state, not just process exit codes.
Dependencies are installed during the image build; the matrix itself is offline.
Docker Desktop on macOS can run this synthetic test without exposing host audio.

| Mode | Fault | Expected baseline | Expected candidate |
| --- | --- | --- | --- |
| -1 | None | 100 callbacks, three opens | Same |
| 0 | XRUN report | Stops at five callbacks, still active | 100 callbacks, RUNNING, three opens |
| 1 | XRUN and one start EIO | Stops at five callbacks, still active | Full restart recovers, three opens |
| 2 | XRUN and persistent start EIO | Stops at five callbacks, still active | Finished callback, inactive stream, three opens |
| 3 | XRUN and one recover EIO | Full restart recovers | Same |

Each resumed stream delivers 389,120 frames after injection (95 callbacks of
4,096 frames at 48 kHz). The block size is not a fixed hardware ring size.
The baseline has three expected failing cases; the aggregate runner succeeds
only when these failures are reproduced and the candidate cases pass.

## Scope

The fixture synthesizes an XRUN indication on null PCM, not a kernel overrun.
Using the same indication on a RUNNING hardware PCM can make recovery return
EBUSY and exercise a different path. Do not reuse this fixture as a hardware
XRUN oracle. Hardware validation should induce a real overrun (for example,
a bounded callback delay) and check recovery, close and reopen separately.

The test does not cover linked duplex, MMAP, hot-unplug, or long-running capture.
An unrecoverable start error is expected to stop the callback stream; it is not
expected to become a Python exception in the caller thread. Lost audio during
an overrun is not reconstructed.
