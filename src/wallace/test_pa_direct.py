#!/usr/bin/env python3
"""Minimal PortAudio test via ctypes — bypasses sounddevice entirely.
Mirrors pa_loopback.cpp as closely as possible."""

import ctypes
import ctypes.util
import struct
import time
import signal
import sys
import os

# Load PortAudio — use explicit path if LD_LIBRARY_PATH is set, else find_library
import os
if os.environ.get('LD_LIBRARY_PATH'):
    # find_library uses ldconfig cache and ignores LD_LIBRARY_PATH
    for d in os.environ['LD_LIBRARY_PATH'].split(':'):
        candidate = os.path.join(d, 'libportaudio.so.2')
        if os.path.exists(candidate):
            lib_path = candidate
            break
    else:
        lib_path = ctypes.util.find_library('portaudio') or 'libportaudio.so.2'
else:
    lib_path = ctypes.util.find_library('portaudio') or 'libportaudio.so.2'
pa = ctypes.CDLL(lib_path)
print(f"Loaded: {lib_path}")

# Constants
paNoError = 0
paInt32 = 0x00000002
paNoFlag = 0
paContinue = 0
paComplete = 1

# Structures (must match portaudio.h exactly)
class PaStreamParameters(ctypes.Structure):
    _fields_ = [
        ('device', ctypes.c_int),
        ('channelCount', ctypes.c_int),
        ('sampleFormat', ctypes.c_ulong),
        ('suggestedLatency', ctypes.c_double),
        ('hostApiSpecificStreamInfo', ctypes.c_void_p),
    ]

class PaStreamCallbackTimeInfo(ctypes.Structure):
    _fields_ = [
        ('inputBufferAdcTime', ctypes.c_double),
        ('currentTime', ctypes.c_double),
        ('outputBufferDacTime', ctypes.c_double),
    ]

class PaStreamInfo(ctypes.Structure):
    _fields_ = [
        ('structVersion', ctypes.c_int),
        ('inputLatency', ctypes.c_double),
        ('outputLatency', ctypes.c_double),
        ('sampleRate', ctypes.c_double),
    ]

# Callback type
PaStreamCallback = ctypes.CFUNCTYPE(
    ctypes.c_int,                    # return
    ctypes.c_void_p,                 # input
    ctypes.c_void_p,                 # output
    ctypes.c_ulong,                  # frameCount
    ctypes.POINTER(PaStreamCallbackTimeInfo),  # timeInfo
    ctypes.c_ulong,                  # statusFlags
    ctypes.c_void_p,                 # userData
)

# Set up function signatures
pa.Pa_Initialize.restype = ctypes.c_int
pa.Pa_Terminate.restype = ctypes.c_int
pa.Pa_GetErrorText.restype = ctypes.c_char_p
pa.Pa_GetErrorText.argtypes = [ctypes.c_int]
pa.Pa_GetDeviceCount.restype = ctypes.c_int
pa.Pa_GetDeviceInfo.restype = ctypes.c_void_p
pa.Pa_OpenStream.restype = ctypes.c_int
pa.Pa_StartStream.restype = ctypes.c_int
pa.Pa_StopStream.restype = ctypes.c_int
pa.Pa_CloseStream.restype = ctypes.c_int
pa.Pa_IsStreamActive.restype = ctypes.c_int
pa.Pa_Sleep.restype = None
pa.Pa_Sleep.argtypes = [ctypes.c_long]
pa.Pa_GetStreamInfo.restype = ctypes.POINTER(PaStreamInfo)
pa.Pa_GetStreamInfo.argtypes = [ctypes.c_void_p]
pa.Pa_GetVersionText.restype = ctypes.c_char_p

running = True

def signal_handler(sig, frame):
    global running
    running = False

signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)

# Callback — mirrors pa_loopback.cpp exactly
callback_count = [0]
CHANNELS = 2

@PaStreamCallback
def loopback_callback(input_ptr, output_ptr, frame_count, time_info, status_flags, user_data):
    global running
    if status_flags:
        sys.stderr.write(f"STATUS: 0x{status_flags:x}\n")

    n_samples = frame_count * CHANNELS
    buf_type = ctypes.c_int32 * n_samples
    in_buf = buf_type.from_address(input_ptr)
    out_buf = buf_type.from_address(output_ptr)

    max_val = 0
    for i in range(min(n_samples, 64)):
        v = abs(in_buf[i])
        if v > max_val:
            max_val = v
        out_buf[i] = in_buf[i]
    # Copy rest without checking
    for i in range(64, n_samples):
        out_buf[i] = in_buf[i]

    callback_count[0] += 1
    if callback_count[0] <= 50 or callback_count[0] % 100 == 0:
        sys.stderr.write(f"cb: frames={frame_count} max={max_val}\n")

    return paContinue if running else paComplete

# Initialize
err = pa.Pa_Initialize()
if err != paNoError:
    print(f"Pa_Initialize failed: {pa.Pa_GetErrorText(err).decode()}")
    sys.exit(1)

print(f"PortAudio: {pa.Pa_GetVersionText().decode()}", flush=True)

# Setup parameters — device 0, same as pa_loopback
device = 0
blocksize = 1024
samplerate = 48000.0

input_params = PaStreamParameters()
input_params.device = device
input_params.channelCount = CHANNELS
input_params.sampleFormat = paInt32
input_params.suggestedLatency = 0.0
input_params.hostApiSpecificStreamInfo = None

output_params = PaStreamParameters()
output_params.device = device
output_params.channelCount = CHANNELS
output_params.sampleFormat = paInt32
output_params.suggestedLatency = 0.0
output_params.hostApiSpecificStreamInfo = None

stream = ctypes.c_void_p()
err = pa.Pa_OpenStream(
    ctypes.byref(stream),
    ctypes.byref(input_params),
    ctypes.byref(output_params),
    ctypes.c_double(samplerate),
    ctypes.c_ulong(blocksize),
    ctypes.c_ulong(paNoFlag),
    loopback_callback,
    None,
)
if err != paNoError:
    print(f"Pa_OpenStream failed: {pa.Pa_GetErrorText(err).decode()}")
    pa.Pa_Terminate()
    sys.exit(1)

info = pa.Pa_GetStreamInfo(stream)
if info:
    print(f"Input latency:  {info.contents.inputLatency * 1000:.3f} ms")
    print(f"Output latency: {info.contents.outputLatency * 1000:.3f} ms")
    print(f"Sample rate:    {info.contents.sampleRate:.0f}", flush=True)

print("Calling Pa_StartStream...", flush=True)
err = pa.Pa_StartStream(stream)
print(f"Pa_StartStream returned: {err}", flush=True)
if err != paNoError:
    print(f"Pa_StartStream failed: {pa.Pa_GetErrorText(err).decode()}", flush=True)
    pa.Pa_CloseStream(stream)
    pa.Pa_Terminate()
    sys.exit(1)

print("Loopback running — Ctrl+C to stop", flush=True)

while running and pa.Pa_IsStreamActive(stream):
    pa.Pa_Sleep(100)

pa.Pa_StopStream(stream)
pa.Pa_CloseStream(stream)
pa.Pa_Terminate()
print(f"Done. {callback_count[0]} callbacks processed.")
