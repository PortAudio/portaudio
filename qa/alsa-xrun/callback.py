"""Exercise one non-MMAP capture callback stream against the ALSA null PCM."""

import ctypes
import json
import os
import sys
import threading

import sounddevice as sd


class Probe:
    def __init__(self):
        self.count = 0
        self.after = 0
        self.overflow = 0

        self.state = None
        self.done = threading.Event()
        self.mode = int(os.environ.get("FAULT_MODE", "0"))
        self.inject = ctypes.CDLL(None)
        self.inject.PaTest_Arm.argtypes = [ctypes.c_int]
        self.inject.PaTest_Arm.restype = None
        self.inject.PaTest_Reset.argtypes = []
        self.inject.PaTest_Reset.restype = None
        self.inject.PaTest_State.argtypes = []
        self.inject.PaTest_State.restype = ctypes.c_int

    def callback(self, data, frames, timing, status):
        self.count += 1
        self.overflow += bool(status.input_overflow)
        if self.count == 5 and self.mode >= 0:
            self.inject.PaTest_Arm(self.mode)
        if self.count > 5:
            self.after += frames
            if self.mode >= 0:
                self.state = self.inject.PaTest_State()
        if self.count >= 100:
            raise sd.CallbackStop

    def run(self):
        for trial in range(3):
            self.count = self.after = self.overflow = 0
            self.state = None
            self.done.clear()
            if self.mode >= 0:
                self.inject.PaTest_Reset()
            with sd.RawInputStream(
                samplerate=48000,
                channels=1,
                dtype="int16",
                blocksize=4096,
                device="default",
                callback=self.callback,
                finished_callback=self.done.set,
            ) as stream:
                ended = self.done.wait(15)
                print(
                    json.dumps(
                        {
                            "stage": "before_teardown",
                            "trial": trial,
                            "callbacks": self.count,
                            "ended": ended,
                            "state": self.state,
                        }
                    ),
                    flush=True,
                )
                active = stream.active
                result = {
                    "trial": trial,
                    "mode": self.mode,
                    "callbacks": self.count,
                    "after_frames": self.after,
                    "state": self.state,
                    "overflow": self.overflow,
                    "ended": ended,
                    "active": active,
                }
            if self.mode == 2:
                passed = ended and self.count == 5 and not active
            else:
                passed = ended and self.count == 100 and self.after == 95 * 4096
                if self.mode >= 0:
                    passed = passed and self.state == 3
            result["pass"] = bool(passed)
            print(json.dumps(result), flush=True)
            if not passed:
                return 1
        return 0


if __name__ == "__main__":
    print(
        json.dumps(
            {
                "version": sd.__version__,
                "portaudio": sd.get_portaudio_version(),
                "library": sd._libname,
            }
        ),
        flush=True,
    )
    sys.exit(Probe().run())
