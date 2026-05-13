#!/usr/bin/env python3
"""Compare PaStreamParameters struct bytes between ctypes and cffi."""

import ctypes
import sys

# === CTYPES version ===
class PaStreamParameters_ct(ctypes.Structure):
    _fields_ = [
        ('device', ctypes.c_int),
        ('channelCount', ctypes.c_int),
        ('sampleFormat', ctypes.c_ulong),
        ('suggestedLatency', ctypes.c_double),
        ('hostApiSpecificStreamInfo', ctypes.c_void_p),
    ]

ct_params = PaStreamParameters_ct()
ct_params.device = 0
ct_params.channelCount = 2
ct_params.sampleFormat = 0x00000002  # paInt32
ct_params.suggestedLatency = 0.0
ct_params.hostApiSpecificStreamInfo = None

ct_bytes = bytes(ct_params)
print(f"CTYPES size: {ctypes.sizeof(ct_params)}")
print(f"CTYPES hex:  {ct_bytes.hex()}")
print(f"CTYPES fields:")
for name, _ in PaStreamParameters_ct._fields_:
    off = PaStreamParameters_ct.__dict__[name].offset
    sz = PaStreamParameters_ct.__dict__[name].size
    print(f"  {name}: offset={off} size={sz}")

# === CFFI version ===
import sounddevice as sd
_ffi = sd._ffi
_lib = sd._lib

cf_params = _ffi.new('PaStreamParameters*')
cf_params.device = 0
cf_params.channelCount = 2
cf_params.sampleFormat = _lib.paInt32
cf_params.suggestedLatency = 0.0
cf_params.hostApiSpecificStreamInfo = _ffi.NULL

cf_bytes = bytes(_ffi.buffer(cf_params))
print(f"\nCFFI size: {_ffi.sizeof('PaStreamParameters')}")
print(f"CFFI hex:  {cf_bytes.hex()}")
print(f"CFFI paInt32 = {_lib.paInt32}")

# === Compare ===
print(f"\nMatch: {ct_bytes == cf_bytes}")
if ct_bytes != cf_bytes:
    print("MISMATCH! Byte-by-byte:")
    for i in range(max(len(ct_bytes), len(cf_bytes))):
        a = ct_bytes[i] if i < len(ct_bytes) else None
        b = cf_bytes[i] if i < len(cf_bytes) else None
        if a != b:
            print(f"  byte {i}: ctypes=0x{a:02x} cffi=0x{b:02x}")
