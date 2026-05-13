#!/usr/bin/env python3
"""Test: does cffi's dlopen alone (no sounddevice) cause zeros?
Loads library via cffi first, then uses ctypes for everything."""

import ctypes
import sys
import signal
import os

# Step 1: Load library via cffi FIRST (mimics what sounddevice does)
import cffi
ffi = cffi.FFI()
print("Loading via cffi.dlopen...", flush=True)
cffi_lib = ffi.dlopen('/usr/local/lib/libportaudio.so.2')
print("cffi dlopen done", flush=True)

# Step 2: Now do everything via ctypes (exact copy of working test_pa_direct.py)
pa = ctypes.CDLL('/usr/local/lib/libportaudio.so.2')
pa.Pa_GetVersionText.restype = ctypes.c_char_p

paNoError = 0
paInt32 = 0x00000002

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

PaStreamCallback = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
    ctypes.c_ulong, ctypes.POINTER(PaStreamCallbackTimeInfo),
    ctypes.c_ulong, ctypes.c_void_p,
)

pa.Pa_Initialize.restype = ctypes.c_int
pa.Pa_Terminate.restype = ctypes.c_int
pa.Pa_GetErrorText.restype = ctypes.c_char_p
pa.Pa_GetErrorText.argtypes = [ctypes.c_int]
pa.Pa_OpenStream.restype = ctypes.c_int
pa.Pa_StartStream.restype = ctypes.c_int
pa.Pa_StopStream.restype = ctypes.c_int
pa.Pa_CloseStream.restype = ctypes.c_int
pa.Pa_IsStreamActive.restype = ctypes.c_int
pa.Pa_Sleep.restype = None
pa.Pa_Sleep.argtypes = [ctypes.c_long]

CHANNELS = 2
BLOCKSIZE = 1024
callback_count = [0]
running = [True]

def sig_handler(s, f):
    running[0] = False
signal.signal(signal.SIGINT, sig_handler)

@PaStreamCallback
def loopback_callback(input_ptr, output_ptr, frame_count, time_info, status_flags, user_data):
    n = frame_count * CHANNELS
    in_buf = (ctypes.c_int32 * n).from_address(input_ptr)
    out_buf = (ctypes.c_int32 * n).from_address(output_ptr)
    max_val = 0
    for i in range(min(n, 64)):
        v = abs(in_buf[i])
        if v > max_val:
            max_val = v
        out_buf[i] = in_buf[i]
    for i in range(64, n):
        out_buf[i] = in_buf[i]
    callback_count[0] += 1
    if callback_count[0] <= 50 or callback_count[0] % 100 == 0:
        sys.stderr.write(f"cb: frames={frame_count} max={max_val}\n")
    return 0 if running[0] else 1

err = pa.Pa_Initialize()
if err != paNoError:
    print(f"Pa_Initialize failed: {pa.Pa_GetErrorText(err).decode()}", flush=True)
    sys.exit(1)
print(f"PortAudio: {pa.Pa_GetVersionText().decode()}", flush=True)

input_params = PaStreamParameters(0, 2, paInt32, 0.0, None)
output_params = PaStreamParameters(0, 2, paInt32, 0.0, None)

stream = ctypes.c_void_p()
err = pa.Pa_OpenStream(
    ctypes.byref(stream),
    ctypes.byref(input_params), ctypes.byref(output_params),
    ctypes.c_double(48000.0), ctypes.c_ulong(BLOCKSIZE),
    ctypes.c_ulong(0), loopback_callback, None,
)
if err != paNoError:
    print(f"Pa_OpenStream failed: {pa.Pa_GetErrorText(err).decode()}", flush=True)
    pa.Pa_Terminate()
    sys.exit(1)

err = pa.Pa_StartStream(stream)
if err != paNoError:
    print(f"Pa_StartStream failed: {err}", flush=True)
    pa.Pa_CloseStream(stream)
    pa.Pa_Terminate()
    sys.exit(1)

print("Loopback running — Ctrl+C to stop", flush=True)
while running[0] and pa.Pa_IsStreamActive(stream):
    pa.Pa_Sleep(100)

pa.Pa_StopStream(stream)
pa.Pa_CloseStream(stream)
pa.Pa_Terminate()
print(f"Done. {callback_count[0]} callbacks.", flush=True)
