# ALSA non-MMAP capture recovery: standalone C reproducer

This opt-in Linux test builds as one C executable. It needs shared PortAudio,
ALSA headers/library, libdl, and POSIX threads. No Python, Docker, separate
preload library or audio hardware is needed for the synthetic tests. It is not
part of the default cross-platform build.

## Build and compare

From the candidate checkout, with CMake, a C compiler and ALSA development
headers installed (for example, `libasound2-dev` on Debian):

```sh
candidate_dir=$PWD
baseline_dir=$(mktemp -d)
work_dir=$(mktemp -d)
git archive 7c45fed9322bd86b5dd0da4547b15d9ac3b9b0ac | tar -x -C "$baseline_dir"
cmake -S "$baseline_dir" -B "$work_dir/baseline" -DPA_USE_ALSA=ON -DPA_USE_JACK=OFF -DPA_BUILD_SHARED_LIBS=ON
cmake --build "$work_dir/baseline"
cmake -S "$candidate_dir" -B "$work_dir/candidate" -DPA_USE_ALSA=ON -DPA_USE_JACK=OFF -DPA_BUILD_SHARED_LIBS=ON
cmake --build "$work_dir/candidate"
for version in baseline candidate; do
    cc -Wall -Wextra -Werror "$candidate_dir/qa/alsa-xrun/patest_alsa_capture_recover.c" \
        -I"$candidate_dir/include" -L"$work_dir/$version" \
        -Wl,-rpath,"$work_dir/$version" -Wl,--export-dynamic \
        -lportaudio -lasound -ldl -lpthread -o "$work_dir/repro-$version"
done
printf 'pcm.null { type null }\n' > "$work_dir/null.conf"
ALSA_CONFIG_PATH="$work_dir/null.conf" "$work_dir/repro-baseline" null 0
ALSA_CONFIG_PATH="$work_dir/null.conf" "$work_dir/repro-candidate" null 0
```

The baseline command is expected to exit 2; continue to run the candidate even
when the baseline fails. The candidate must exit 0 with three passing trials.
A 20-second watchdog bounds each trial, including initialization and teardown.
Each trial observes callback progress for at most about 12 seconds.
Build errors, API setup errors or missing libraries are failures, not a valid
baseline reproduction.

The test reports the overflow flag, callback count, frames after injection,
post-injection PCM state and stream activity. It does not inspect/save samples.
Each successful trial has 100 callbacks and 389120 frames after injection.
The callback block size is 4096 at 48 kHz; this does not fix the ALSA ring size.

## Modes

Pass the mode as the second argument after the ALSA PCM name:

| Mode | Condition | Baseline | Candidate |
| --- | --- | --- | --- |
| -1 | No fault | Pass | Pass |
| 0 | One XRUN report | Stops at five callbacks, exit 2 | Resumes, pass |
| 1 | XRUN and one start EIO | Stops at five callbacks, exit 2 | Full restart recovers, pass |
| 2 | XRUN and persistent start EIO | Stops at five callbacks, exit 2 | Finished/inactive stream, pass |
| 3 | XRUN and one recover EIO | Full restart recovers, pass | Same |
| 4 | Real-input callback delayed one second | Hardware-dependent | Requires observed overflow and resumed callbacks |

Modes -1 through 3 were checked against both revisions on Linux ARM64. Each
successful mode repeats close/reopen three times. `state=-1` means no subsequent
callback observed a state. It is not an ALSA state code.

## How the synthetic fixture works

The executable exports ALSA wrapper symbols (`--export-dynamic`) and resolves
real ALSA functions with `dlsym(RTLD_NEXT, ...)`. Shared dynamic linking is
required. The wrappers reject MMAP access, inject one avail/status XRUN after
callback five, and optionally inject start/recover EIO. The remaining calls
reach ALSA. Fault controls execute on the callback thread; reset occurs after
close. Only one stream may be active. Never link these wrappers into an
application or PortAudio itself.

The null-PCM XRUN is synthetic. A running hardware PCM may reject prepare with
EBUSY if an XRUN is merely reported without a real overrun. Do not use modes
0 through 3 as hardware proof. For physical tests, use mode 4 with an explicitly
selected ALSA PCM (e.g. `hw:0,0`) and the normal ALSA configuration. Mode 4 never
injects avail/status/start/recover faults; it only forces non-MMAP access and
delays the fifth callback. No observed overflow means the test fails rather
than claiming recovery was tested. The new C mode 4 has not yet been validated
on physical input.

The brief mutex protects observations between the callback and main thread;
this diagnostic intentionally delays audio and is not a real-time programming
example. Stream API calls are made on the main thread, never in the finished
callback. Persistent start errors are expected to stop the stream. The test
does not claim to recover USB removal, linked duplex, MMAP, lost audio, or
long-duration recording failures.
