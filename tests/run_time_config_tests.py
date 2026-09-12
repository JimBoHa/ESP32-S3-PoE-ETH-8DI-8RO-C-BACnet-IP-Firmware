#!/usr/bin/env python3
"""Real time configuration parsing/persistence with fault-injected NVS stubs."""
from html.parser import HTMLParser
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = (
    "validation", "defaults", "roundtrip", "identical", "bounds", "corrupt",
    "read-error", "set-error", "commit-error", "open-error", "mutex-error",
    "preservation", "no-memory",
)


class TimezonePresets(HTMLParser):
    def __init__(self):
        super().__init__()
        self.in_presets = False
        self.presets = []

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        if tag == "select":
            self.in_presets = attributes.get("id") == "tTimezonePreset"
        if tag == "option" and self.in_presets and attributes.get("value") != "custom":
            self.presets.append(attributes["value"])

    def handle_endtag(self, tag):
        if tag == "select":
            self.in_presets = False


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="time-config-tests-") as temporary:
        executable = Path(temporary) / "time-config"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-I" + str(ROOT / "tests/restart_store_stubs"),
            "-I" + str(ROOT / "main"), str(ROOT / "tests/test_time_config.c"),
            str(ROOT / "main/time_config.c"), "-o", str(executable),
        ], check=True)
        for scenario in SCENARIOS:
            subprocess.run([str(executable), scenario], check=True, timeout=10)
        parser = TimezonePresets()
        parser.feed((ROOT / "main/web/index.html").read_text(encoding="utf-8"))
        assert len(parser.presets) >= 10
        for preset in parser.presets:
            subprocess.run([str(executable), "preset", preset], check=True, timeout=10)
        print(f"Validated all {len(parser.presets)} UI timezone presets against firmware parser.")
