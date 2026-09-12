#!/usr/bin/env python3
"""Compile real config_store/model with isolated NVS and semaphore fault stubs."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = (
    "missing", "empty", "roundtrip", "identical", "bounds", "corrupt",
    "read-error", "set-error", "commit-error", "preservation", "mutex-error",
)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="restart-store-tests-") as temporary:
        executable = Path(temporary) / "restart-store"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-I" + str(ROOT / "tests/restart_store_stubs"),
            "-I" + str(ROOT / "main"), str(ROOT / "tests/test_restart_store.c"),
            str(ROOT / "main/config_store.c"), str(ROOT / "main/config_model.c"),
            "-o", str(executable),
        ], check=True)
        for scenario in SCENARIOS:
            subprocess.run([str(executable), scenario], check=True, timeout=10)
