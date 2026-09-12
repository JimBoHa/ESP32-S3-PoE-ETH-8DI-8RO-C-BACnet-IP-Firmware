#!/usr/bin/env python3
"""No-network clock arithmetic and boundary tests with sanitizers."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="clock-model-") as temporary:
        executable = Path(temporary) / "clock-model"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-g", "-O1", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", "-I" + str(ROOT / "main"),
                        str(ROOT / "tests/test_clock_model.c"),
                        str(ROOT / "main/clock_model.c"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)
