#!/usr/bin/env python3
"""Hybrid test: sounddevice's cffi-loaded library + ctypes callback.
If this works → cffi callback mechanism is the problem.
If zeros → something about cffi library loading or sounddevice's Pa_Initialize."""

import ctypes
import ctypes.util
import sys
import signal
import os

# Step 1: Import sounddevice (loads PA via cffi, calls Pa_Initialize)
import sounddevice as sd
_ffi = sd._ffi
_lib = sd._lib

version = _ffi.string(_lib.Pa_GetVersionText()).decode()
print(f"CFFI-loaded PA version: {version}", flush=True)

# Step 2: Also load the SAME .so via ctypes (RTLD_NOLOAD gets existing handle)
lib_path = '/usr/local/lib/libportaudio.so.2'
if not os.path.exists(lib_path):
    lib_path = ctypes.util.find_library('portaudio') or 'libportaudio.so.2'
pa = ctypes.CDLL(lib_path)
ct_version = pa.Pa_GetVersionText()
pa.Pa_GetVersionText.restype = ctypes.c_char_p
print(f"CTYPES-loaded PA version: {pa.Pa_GetVersionText().decode()}", flush=True)

# Step 3: Pa_Terminate the cffi init, re-init via ctypes
# (so there's exactly one init, done via ctypes)
_lib.Pa_Terminate()
sd._initialized -= 1

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

err = pa.Pa_Initialize()
if err != 0:
    print(f"Pa_Initialize failed: {err}", flush=True)
    sys.exit(1)
print("Pa_Initialize via ctypes: OK", flush=True)

# Step 4: ctypes structs and callback (same as working test_pa_direct.py)
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

input_params = PaStreamParameters(0, 2, 0x2, 0.0, None)
output_params = PaStreamParameters(0, 2, 0x2, 0.0, None)

stream = ctypes.c_void_p()
err = pa.Pa_OpenStream(
    ctypes.byref(stream),
    ctypes.byref(input_params), ctypes.byref(output_params),
    ctypes.c_double(48000.0), ctypes.c_ulong(BLOCKSIZE),
    ctypes.c_ulong(0),
    loopback_callback, None,
)
if err != 0:
    pa.Pa_GetErrorText.restype = ctypes.c_char_p
    print(f"Pa_OpenStream failed: {pa.Pa_GetErrorText(err).decode()}", flush=True)
    pa.Pa_Terminate()
    sys.exit(1)

err = pa.Pa_StartStream(stream)
if err != 0:
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
