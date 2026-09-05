#!/usr/bin/env python3
"""Host tests for the BACnet hardware-in-the-loop runner."""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import unittest

from tools import bacnet_hil_test


class FakePriorityValue:
    def __init__(self, choice: str) -> None:
        self._choice = choice


def args(**overrides: object) -> argparse.Namespace:
    values = {
        "local_address": "192.168.75.191/24",
        "device_address": "192.168.75.154",
        "device_instance": 599153,
        "client_instance": 4194001,
        "timeout": 4.0,
        "relay_seconds": 3.0,
        "exercise_relays": False,
        "relay_safety_confirmation": None,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class BacnetHilTests(unittest.TestCase):
    def test_expected_object_map(self) -> None:
        objects = bacnet_hil_test.expected_object_identifiers(599153)
        self.assertEqual(len(objects), 28)
        self.assertIn("device,599153", objects)
        self.assertIn("binary-input,8", objects)
        self.assertIn("binary-output,8", objects)
        self.assertIn("network-port,1", objects)

    def test_priority_array_clear(self) -> None:
        clear = [FakePriorityValue("null") for _ in range(16)]
        self.assertTrue(bacnet_hil_test.priority_array_is_clear(clear))
        clear[7] = FakePriorityValue("enumerated")
        self.assertFalse(bacnet_hil_test.priority_array_is_clear(clear))
        self.assertFalse(bacnet_hil_test.priority_array_is_clear(clear[:15]))

    def test_relay_safety_confirmation_is_mandatory(self) -> None:
        with self.assertRaisesRegex(bacnet_hil_test.HilError, "loads-disconnected"):
            bacnet_hil_test.validate_args(args(exercise_relays=True))
        bacnet_hil_test.validate_args(
            args(
                exercise_relays=True,
                relay_safety_confirmation=bacnet_hil_test.RELAY_CONFIRMATION,
            )
        )

    def test_network_and_identity_validation(self) -> None:
        with self.assertRaisesRegex(bacnet_hil_test.HilError, "subnet"):
            bacnet_hil_test.validate_args(args(device_address="192.168.76.154"))
        with self.assertRaisesRegex(bacnet_hil_test.HilError, "must differ"):
            bacnet_hil_test.validate_args(args(client_instance=599153))
        with self.assertRaisesRegex(bacnet_hil_test.HilError, "between 3 and 30"):
            bacnet_hil_test.validate_args(args(relay_seconds=2.9))

    def test_report_is_machine_readable(self) -> None:
        report = bacnet_hil_test.TestReport(
            started_at="2026-01-01T00:00:00Z",
            target={"device_address": "192.0.2.1", "device_instance": 1},
            options={"exercise_relays": False},
        )
        with redirect_stdout(io.StringIO()):
            report.add("discovery", "pass", "one device")
            report.add("relay", "skip", "not authorized")
        report.finish()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            bacnet_hil_test.write_report(path, report)
            saved = json.loads(path.read_text(encoding="utf-8"))
        self.assertTrue(saved["summary"]["success"])
        self.assertEqual(saved["summary"]["passed"], 1)
        self.assertEqual(saved["summary"]["skipped"], 1)

    def test_failed_report_summary(self) -> None:
        report = bacnet_hil_test.TestReport(
            started_at="2026-01-01T00:00:00Z",
            target={},
            options={},
        )
        with redirect_stdout(io.StringIO()):
            report.add("internal error", "fail", "test failure")
        saved = report.serializable()
        self.assertFalse(saved["summary"]["success"])
        self.assertEqual(saved["summary"]["failed"], 1)


if __name__ == "__main__":
    unittest.main()
