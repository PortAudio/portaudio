#!/usr/bin/env python3
"""Diagnostic: dump exact Pa_OpenStream params from sd.Stream vs our direct call."""

import sys
import sounddevice as sd

_ffi = sd._ffi
_lib = sd._lib

# Monkey-patch Pa_OpenStream to log params
_real_open = _lib.Pa_OpenStream

def dump_params(tag, ipar, opar, sr, bs, flags):
    for name, par in [('input', ipar), ('output', opar)]:
        if par == _ffi.NULL:
            print(f"  {tag} {name}: NULL", flush=True)
        else:
            print(f"  {tag} {name}: device={par.device} ch={par.channelCount} "
                  f"fmt={par.sampleFormat:#x} latency={par.suggestedLatency} "
                  f"hostInfo={'NULL' if par.hostApiSpecificStreamInfo == _ffi.NULL else 'SET'}",
                  flush=True)
    print(f"  {tag} sr={sr} bs={bs} flags={flags:#x}", flush=True)

# === Our working params (from test_sd_import.py) ===
print("=== Our direct params ===", flush=True)
ip = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
op = _ffi.new('PaStreamParameters*', (0, 2, _lib.paInt32, 0.0, _ffi.NULL))
dump_params('DIRECT', ip, op, 48000.0, 1024, 0)

# === sd.Stream params ===
print("\n=== sd.Stream params ===", flush=True)
# Intercept by checking _get_stream_parameters output
from sounddevice import _get_stream_parameters, _split
device = 0
channels = 2
dtype = 'int32'
latency = 0
samplerate = 48000
blocksize = 1024

idevice, odevice = _split(device)
ichannels, ochannels = _split(channels)
idtype, odtype = _split(dtype)
ilatency, olatency = _split(latency)

ipar, idtype2, isize, isr = _get_stream_parameters('input', idevice, ichannels, idtype, ilatency, None, samplerate)
opar, odtype2, osize, osr = _get_stream_parameters('output', odevice, ochannels, odtype, olatency, None, samplerate)

dump_params('SD', ipar, opar, isr, blocksize, 0)

print(f"\n  SD idtype={idtype2} odtype={odtype2} isize={isize} osize={osize}", flush=True)

# Check raw bytes
print("\n=== Raw struct bytes ===", flush=True)
ours_i = bytes(_ffi.buffer(ip, _ffi.sizeof('PaStreamParameters')))
ours_o = bytes(_ffi.buffer(op, _ffi.sizeof('PaStreamParameters')))
sd_i = bytes(_ffi.buffer(ipar, _ffi.sizeof('PaStreamParameters')))
sd_o = bytes(_ffi.buffer(opar, _ffi.sizeof('PaStreamParameters')))
print(f"  DIRECT input:  {ours_i.hex()}", flush=True)
print(f"  SD     input:  {sd_i.hex()}", flush=True)
print(f"  DIRECT output: {ours_o.hex()}", flush=True)
print(f"  SD     output: {sd_o.hex()}", flush=True)
print(f"  Input match:  {ours_i == sd_i}", flush=True)
print(f"  Output match: {ours_o == sd_o}", flush=True)
