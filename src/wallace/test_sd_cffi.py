#!/usr/bin/env python3
"""Use sounddevice's cffi _lib/_ffi to call Pa_OpenStream directly.
Bypasses sounddevice's Stream class but uses same cffi callback mechanism.
If this works → bug is in sounddevice's Stream setup.
If zeros → bug is in cffi callback or Pa_Initialize interaction."""

import sys
sys.stderr.write("Importing sounddevice...\n")
import sounddevice as sd

_ffi = sd._ffi
_lib = sd._lib

# Undo sounddevice's Pa_Initialize (which redirected stderr to /dev/null)
# and re-initialize cleanly
sys.stderr.write("Re-initializing PortAudio (undoing sounddevice's init)...\n")
_lib.Pa_Terminate()
sd._initialized -= 1
err = _lib.Pa_Initialize()
sd._initialized += 1
if err != _lib.paNoError:
    sys.stderr.write(f"Pa_Initialize failed: {err}\n")
    sys.exit(1)

sys.stderr.write(f"Library loaded, Pa re-initialized\n")
sys.stderr.write(f"Version: {_ffi.string(_lib.Pa_GetVersionText()).decode()}\n")
sys.stderr.flush()

# Build params exactly like test_pa_direct.py
input_params = _ffi.new('PaStreamParameters*')
input_params.device = 0
input_params.channelCount = 2
input_params.sampleFormat = _lib.paInt32
input_params.suggestedLatency = 0.0
input_params.hostApiSpecificStreamInfo = _ffi.NULL

output_params = _ffi.new('PaStreamParameters*')
output_params.device = 0
output_params.channelCount = 2
output_params.sampleFormat = _lib.paInt32
output_params.suggestedLatency = 0.0
output_params.hostApiSpecificStreamInfo = _ffi.NULL

CHANNELS = 2
BLOCKSIZE = 1024
callback_count = [0]
running = [True]

import signal
def sig_handler(s, f):
    running[0] = False
signal.signal(signal.SIGINT, sig_handler)

@_ffi.callback('PaStreamCallback', error=_lib.paAbort)
def loopback_callback(input_ptr, output_ptr, frame_count, time_info, status_flags, user_data):
    if status_flags:
        sys.stderr.write(f"STATUS: 0x{status_flags:x}\n")

    # Copy input to output and check max value
    n = frame_count * CHANNELS
    in_buf = _ffi.cast('int32_t*', input_ptr)
    out_buf = _ffi.cast('int32_t*', output_ptr)

    max_val = 0
    for i in range(min(n, 64)):
        v = in_buf[i] if in_buf[i] >= 0 else -in_buf[i]
        if v > max_val:
            max_val = v
        out_buf[i] = in_buf[i]
    for i in range(64, n):
        out_buf[i] = in_buf[i]

    callback_count[0] += 1
    if callback_count[0] <= 50 or callback_count[0] % 100 == 0:
        sys.stderr.write(f"cb: frames={frame_count} max={max_val}\n")

    return _lib.paContinue if running[0] else _lib.paComplete

stream_ptr = _ffi.new('PaStream**')
err = _lib.Pa_OpenStream(
    stream_ptr,
    input_params,
    output_params,
    48000.0,
    BLOCKSIZE,
    _lib.paNoFlag,
    loopback_callback,
    _ffi.NULL,
)
if err != _lib.paNoError:
    sys.stderr.write(f"Pa_OpenStream failed: {_ffi.string(_lib.Pa_GetErrorText(err)).decode()}\n")
    sys.exit(1)

stream = stream_ptr[0]
info = _lib.Pa_GetStreamInfo(stream)
if info != _ffi.NULL:
    sys.stderr.write(f"Input latency:  {info.inputLatency * 1000:.3f} ms\n")
    sys.stderr.write(f"Output latency: {info.outputLatency * 1000:.3f} ms\n")
    sys.stderr.write(f"Sample rate:    {info.sampleRate:.0f}\n")

err = _lib.Pa_StartStream(stream)
if err != _lib.paNoError:
    sys.stderr.write(f"Pa_StartStream failed: {_ffi.string(_lib.Pa_GetErrorText(err)).decode()}\n")
    _lib.Pa_CloseStream(stream)
    sys.exit(1)

sys.stderr.write("Loopback running — Ctrl+C to stop\n")
sys.stderr.flush()

while running[0] and _lib.Pa_IsStreamActive(stream):
    _lib.Pa_Sleep(100)

_lib.Pa_StopStream(stream)
_lib.Pa_CloseStream(stream)
sys.stderr.write(f"Done. {callback_count[0]} callbacks.\n")
