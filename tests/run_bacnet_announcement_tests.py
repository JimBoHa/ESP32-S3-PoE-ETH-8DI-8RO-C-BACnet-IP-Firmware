#!/usr/bin/env python3
"""Run the isolated startup/clock-wait scheduler, without device access."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="bacnet-announcement-") as temporary:
        executable = Path(temporary) / "announcement"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-g", "-O1", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", "-I" + str(ROOT / "main"),
                        str(ROOT / "tests/test_bacnet_announcement.c"),
                        str(ROOT / "main/bacnet_announcement.c"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)
