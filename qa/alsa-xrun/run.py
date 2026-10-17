"""Build two read-only source trees and check non-MMAP callback recovery."""

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path


class Matrix:
    def __init__(self, baseline: Path, candidate: Path, output: Path):
        self.baseline = baseline.resolve()
        self.candidate = candidate.resolve()
        self.output = output.resolve()
        self.fixture = Path(__file__).resolve().parent

    def run(self):
        self.output.mkdir(parents=True, exist_ok=False)
        results = []
        with tempfile.TemporaryDirectory(prefix="pa-xrun-") as directory:
            temporary = Path(directory)
            injection = temporary / "inject.so"
            subprocess.run(
                [
                    "cc",
                    "-shared",
                    "-fPIC",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(self.fixture / "inject.c"),
                    "-o",
                    str(injection),
                    "-ldl",
                    "-lasound",
                ],
                check=True,
            )
            config = temporary / "alsa.conf"
            config.write_text(
                "pcm.null { type null }\npcm.default { type plug slave { pcm null channels 1 rate 48000 } }\n"
            )
            for label, source in (
                ("baseline", self.baseline),
                ("candidate", self.candidate),
            ):
                build = temporary / label
                with (self.output / f"{label}-build.log").open("w") as log:
                    subprocess.run(
                        [
                            "cmake",
                            "-S",
                            str(source),
                            "-B",
                            str(build),
                            "-DPA_USE_JACK=OFF",
                            "-DPA_USE_ALSA=ON",
                        ],
                        check=True,
                        stdout=log,
                        stderr=log,
                    )
                    subprocess.run(
                        ["cmake", "--build", str(build), "-j2"],
                        check=True,
                        stdout=log,
                        stderr=log,
                    )
                environment = {
                    **os.environ,
                    "LD_LIBRARY_PATH": str(build),
                    "LD_PRELOAD": str(injection),
                    "ALSA_CONFIG_PATH": str(config),
                    "PYTHONUNBUFFERED": "1",
                }
                for mode in (-1, 0, 1, 2, 3):
                    environment["FAULT_MODE"] = str(mode)
                    log_path = self.output / f"{label}-{mode}.log"
                    with log_path.open("w") as log:
                        result = subprocess.run(
                            ["python", str(self.fixture / "callback.py")],
                            env=environment,
                            stdout=log,
                            stderr=log,
                            timeout=50,
                            check=False,
                        )
                    rows = [
                        json.loads(line)
                        for line in log_path.read_text().splitlines()
                        if line.startswith("{") and '"pass"' in line
                    ]
                    expected_failure = label == "baseline" and mode in (0, 1, 2)
                    if expected_failure:
                        passed = (
                            result.returncode == 1
                            and len(rows) == 1
                            and not rows[0]["pass"]
                            and rows[0]["callbacks"] == 5
                            and rows[0]["after_frames"] == 0
                            and rows[0]["active"]
                        )
                    else:
                        passed = (
                            result.returncode == 0
                            and len(rows) == 3
                            and all(row["pass"] for row in rows)
                        )
                    entry = {
                        "source": label,
                        "mode": mode,
                        "expected_baseline_failure": expected_failure,
                        "exit": result.returncode,
                        "pass": passed,
                    }
                    results.append(entry)
                    print(json.dumps(entry), flush=True)
                    (self.output / "results.json").write_text(
                        json.dumps(results, indent=2) + "\n"
                    )
                    if not passed:
                        raise RuntimeError(f"Unexpected outcome; inspect {log_path}")
        print(
            "PASS: all ten baseline/candidate outcomes match expectations", flush=True
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    Matrix(args.baseline, args.candidate, args.output).run()
