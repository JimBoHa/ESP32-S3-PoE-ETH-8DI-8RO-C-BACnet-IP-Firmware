#!/usr/bin/env python3
"""Run actual clock_service.c with deterministic, entirely offline IDF stubs.

Includes the service implementation to seed simulated RTC RAM and drive its
otherwise-infinite owner task. POSIX clock writes are intercepted; no host clock,
network, device, or NVS is touched. Host libc timezone tests do not substitute for
the pinned newlib firmware build or hardware RTC/reconnect acceptance.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = (
    "reject-before-set", "accepted-clock", "set-failure", "snapshot-timeout",
    "retained-sw", "retained-sleep", "retained-power", "retained-brownout",
    "retained-panic", "retained-crc", "retained-stale", "retained-future",
    "retained-bad-time", "retained-get-failure", "retained-boundary",
    "timezone", "owner-reconfigure", "owner-retry", "offline-startup",
    "dns-config-change", "dns-link-loss", "dns-network-change", "dns-failure",
    "dns-invalid-address", "dns-hourly-refresh",
)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="clock-service-tests-") as temporary:
        executable = Path(temporary) / "clock-service"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE",
            "-D_DARWIN_C_SOURCE", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I" + str(ROOT / "tests/clock_service_stubs"),
            "-I" + str(ROOT / "main"), str(ROOT / "tests/test_clock_service.c"),
            str(ROOT / "main/clock_model.c"), "-o", str(executable),
        ], check=True)
        for scenario in SCENARIOS:
            subprocess.run([str(executable), scenario], check=True, timeout=10)
