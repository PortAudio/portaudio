#!/usr/bin/env python3
"""Blocking PortAudio read test — no callback, no GIL interaction.
If this produces zeros, the issue is in PortAudio stream setup from Python."""

import ctypes
import ctypes.util
import signal
import sys

pa = ctypes.CDLL(ctypes.util.find_library('portaudio') or 'libportaudio.so.2')
print(f"Loaded: {pa._name}", flush=True)

paNoError = 0
paInt32 = 0x00000002
paNoFlag = 0

class PaStreamParameters(ctypes.Structure):
    _fields_ = [
        ('device', ctypes.c_int),
        ('channelCount', ctypes.c_int),
        ('sampleFormat', ctypes.c_ulong),
        ('suggestedLatency', ctypes.c_double),
        ('hostApiSpecificStreamInfo', ctypes.c_void_p),
    ]

class PaStreamInfo(ctypes.Structure):
    _fields_ = [
        ('structVersion', ctypes.c_int),
        ('inputLatency', ctypes.c_double),
        ('outputLatency', ctypes.c_double),
        ('sampleRate', ctypes.c_double),
    ]

pa.Pa_Initialize.restype = ctypes.c_int
pa.Pa_Terminate.restype = ctypes.c_int
pa.Pa_GetErrorText.restype = ctypes.c_char_p
pa.Pa_GetErrorText.argtypes = [ctypes.c_int]
pa.Pa_OpenStream.restype = ctypes.c_int
pa.Pa_StartStream.restype = ctypes.c_int
pa.Pa_ReadStream.restype = ctypes.c_int
pa.Pa_ReadStream.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
pa.Pa_WriteStream.restype = ctypes.c_int
pa.Pa_WriteStream.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
pa.Pa_StopStream.restype = ctypes.c_int
pa.Pa_CloseStream.restype = ctypes.c_int
pa.Pa_GetVersionText.restype = ctypes.c_char_p
pa.Pa_GetStreamInfo.restype = ctypes.POINTER(PaStreamInfo)
pa.Pa_GetStreamInfo.argtypes = [ctypes.c_void_p]

running = True
def sig_handler(s, f):
    global running
    running = False
signal.signal(signal.SIGINT, sig_handler)

CHANNELS = 2
BLOCKSIZE = 1024

err = pa.Pa_Initialize()
if err != paNoError:
    print(f"Pa_Initialize failed: {pa.Pa_GetErrorText(err).decode()}")
    sys.exit(1)

print(f"PortAudio: {pa.Pa_GetVersionText().decode()}", flush=True)

# Input-only stream for simplicity
input_params = PaStreamParameters()
input_params.device = 0
input_params.channelCount = CHANNELS
input_params.sampleFormat = paInt32
input_params.suggestedLatency = 0.0
input_params.hostApiSpecificStreamInfo = None

stream = ctypes.c_void_p()
err = pa.Pa_OpenStream(
    ctypes.byref(stream),
    ctypes.byref(input_params),
    None,  # no output — input only
    ctypes.c_double(48000.0),
    ctypes.c_ulong(BLOCKSIZE),
    ctypes.c_ulong(paNoFlag),
    None,  # no callback — blocking mode
    None,
)
if err != paNoError:
    print(f"Pa_OpenStream failed: {pa.Pa_GetErrorText(err).decode()}", flush=True)
    pa.Pa_Terminate()
    sys.exit(1)

info = pa.Pa_GetStreamInfo(stream)
if info:
    print(f"Input latency: {info.contents.inputLatency * 1000:.3f} ms", flush=True)

err = pa.Pa_StartStream(stream)
if err != paNoError:
    print(f"Pa_StartStream failed: {pa.Pa_GetErrorText(err).decode()}", flush=True)
    pa.Pa_CloseStream(stream)
    pa.Pa_Terminate()
    sys.exit(1)

print("Blocking read loop — Ctrl+C to stop", flush=True)

buf = (ctypes.c_int32 * (BLOCKSIZE * CHANNELS))()
count = 0
while running and count < 50:
    err = pa.Pa_ReadStream(stream, buf, BLOCKSIZE)
    if err != paNoError:
        print(f"Pa_ReadStream error: {pa.Pa_GetErrorText(err).decode()}", flush=True)
        break
    max_val = max(abs(buf[i]) for i in range(min(64, BLOCKSIZE * CHANNELS)))
    count += 1
    print(f"read #{count}: max={max_val}", flush=True)

pa.Pa_StopStream(stream)
pa.Pa_CloseStream(stream)
pa.Pa_Terminate()
print("Done.", flush=True)
