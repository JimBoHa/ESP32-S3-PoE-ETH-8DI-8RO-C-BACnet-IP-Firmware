#!/usr/bin/env python3
"""Fault-inject the actual board driver using deterministic, no-device IDF stubs.

Default mode requires the repaired behavior. --expect-baseline documents the
failure behavior of an explicitly supplied pre-fix --source, not a passing fix.
The model checks expander registers/pin direction, not relay contacts or timing
of real FreeRTOS scheduling, electrical hardware, or a BACnet application.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = (
    "startup-off", "startup-discarded-latch", "warm-start-polarity-failure", "normal", "mutex-timeout",
    "mutex-latest-wins", "mutex-off-recovery",
    "i2c-retry", "expander-reset", "read-failure",
    "config-write-failure", "latch-mismatch",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "main/board_io.c")
    parser.add_argument("--expect-baseline", action="store_true")
    args = parser.parse_args()
    if args.expect_baseline and args.source.resolve() == (ROOT / "main/board_io.c").resolve():
        parser.error("baseline reproduction requires a preserved pre-fix --source")
    with tempfile.TemporaryDirectory(prefix="board-io-tests-") as temporary:
        executable = Path(temporary) / "board-io"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            *([] if args.expect_baseline else ["-DBOARD_IO_EXPECT_FIXED=1"]),
            "-fno-omit-frame-pointer", "-I" + str(ROOT / "tests/board_io_stubs"),
            "-I" + str(ROOT / "main"), str(ROOT / "tests/test_board_io.c"),
            str(args.source.resolve()), "-o", str(executable),
        ], check=True)
        mode = "baseline" if args.expect_baseline else "fixed"
        print("Board I/O mode:", mode, "source:", args.source, flush=True)
        for scenario in SCENARIOS:
            subprocess.run([str(executable), scenario, mode], check=True, timeout=10)


if __name__ == "__main__":
    main()
