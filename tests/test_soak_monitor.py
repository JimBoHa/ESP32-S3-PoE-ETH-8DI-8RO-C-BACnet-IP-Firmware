#!/usr/bin/env python3
"""Host tests for the long-running health monitor."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import tempfile
import unittest

from tools import soak_monitor


I_AM = bytes.fromhex(
    "81 0a 00 15 01 00 10 00 c4 02 09 24 71 22 05 c4 91 03 22 01 04"
)


def healthy_status() -> dict[str, object]:
    return {
        "firmware_version": "0.13.0",
        "build_date": "Sep  4 2026",
        "running_partition": "ota_1",
        "reboot_count": 11,
        "last_reset_reason": "software",
        "last_reset_reason_code": 3,
        "product": "ESP32 BACnet I/O",
        "ip_address": "192.168.75.154",
        "bacnet_device_instance": 599153,
        "bacnet_vendor_id": 260,
        "bacnet_udp_port": 47808,
        "ethernet_link": True,
        "ipv4_assigned": True,
        "bacnet_running": True,
        "relay_controller_healthy": True,
        "rtc_present": True,
        "relay_outputs_mask": 0,
        "relay_commands_mask": 0,
        "relay_active_priorities": [0] * 8,
        "free_heap_bytes": 275000,
        "minimum_free_heap_bytes": 268000,
        "uptime_seconds": 100,
        "bacnet_packets_received": 50,
    }


def healthy_config() -> dict[str, object]:
    return {
        "database_revision": 3,
        "device_instance": 599153,
        "vendor_id": 260,
        "bacnet_port": 47808,
        "device_name": "BACnet IO 599153",
    }


def bacnet_response() -> dict[str, object]:
    return {
        "device_instance": 599153,
        "max_apdu": 1476,
        "segmentation": 3,
        "vendor_id": 260,
    }


class SoakMonitorTests(unittest.TestCase):
    def test_parse_real_i_am(self) -> None:
        parsed = soak_monitor.parse_i_am(I_AM)
        self.assertEqual(parsed["device_instance"], 599153)
        self.assertEqual(parsed["max_apdu"], 1476)
        self.assertEqual(parsed["segmentation"], 3)
        self.assertEqual(parsed["vendor_id"], 260)

    def test_i_am_rejects_bad_length_and_service(self) -> None:
        with self.assertRaisesRegex(soak_monitor.SoakError, "length mismatch"):
            soak_monitor.parse_i_am(I_AM[:-1])
        wrong_service = bytearray(I_AM)
        wrong_service[7] = 1
        with self.assertRaisesRegex(soak_monitor.SoakError, "not an I-Am"):
            soak_monitor.parse_i_am(bytes(wrong_service))

    def test_config_fingerprint_is_order_independent(self) -> None:
        first = {"a": 1, "b": [2, 3]}
        second = {"b": [2, 3], "a": 1}
        self.assertEqual(
            soak_monitor.config_fingerprint(first),
            soak_monitor.config_fingerprint(second),
        )

    def test_schedule_includes_full_duration_boundary(self) -> None:
        self.assertEqual(
            list(soak_monitor.sample_schedule(16.0, 5.0)),
            [0.0, 5.0, 10.0, 15.0, 16.0],
        )
        self.assertEqual(
            list(soak_monitor.sample_schedule(60.0, 60.0)),
            [0.0, 60.0],
        )

    def test_healthy_sample_has_no_alerts(self) -> None:
        status = healthy_status()
        config = healthy_config()
        baseline = soak_monitor.Baseline.from_values(status, config)
        next_status = dict(status)
        next_status["uptime_seconds"] = 160
        next_status["bacnet_packets_received"] = 51
        alerts = soak_monitor.evaluate_sample(
            baseline,
            status,
            next_status,
            config,
            bacnet_response(),
            expected_relay_mask=0,
            minimum_heap_bytes=200000,
        )
        self.assertEqual(alerts, [])

    def test_reboot_relay_heap_and_config_changes_alert(self) -> None:
        status = healthy_status()
        config = healthy_config()
        baseline = soak_monitor.Baseline.from_values(status, config)
        changed = dict(status)
        changed.update(
            {
                "reboot_count": 12,
                "uptime_seconds": 1,
                "relay_outputs_mask": 1,
                "relay_commands_mask": 1,
                "relay_active_priorities": [8] + [0] * 7,
                "free_heap_bytes": 190000,
                "minimum_free_heap_bytes": 180000,
            }
        )
        changed_config = dict(config)
        changed_config["database_revision"] = 4
        alerts = soak_monitor.evaluate_sample(
            baseline,
            status,
            changed,
            changed_config,
            bacnet_response(),
            expected_relay_mask=0,
            minimum_heap_bytes=200000,
        )
        joined = " ".join(alerts)
        for expected in (
            "reboot_count-changed",
            "database-revision-changed",
            "configuration-content-changed",
            "relay-outputs-mask",
            "relay-priorities-active",
            "free-heap-below-floor",
            "uptime_seconds-decreased",
        ):
            self.assertIn(expected, joined)

    def test_jsonl_log_refuses_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "soak.jsonl"
            log = soak_monitor.JsonlLog(path)
            log.write({"type": "sample", "sequence": 0})
            log.close()
            saved = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(saved["sequence"], 0)
            with self.assertRaisesRegex(soak_monitor.SoakError, "overwrite"):
                soak_monitor.JsonlLog(path)

    def test_argument_safety(self) -> None:
        args = argparse.Namespace(
            device_address="192.168.75.154",
            device_instance=599153,
            bacnet_port=47808,
            duration=60.0,
            interval=10.0,
            timeout=4.0,
            summary_every=1,
            expected_relay_mask=0,
            minimum_heap_bytes=200000,
        )
        soak_monitor.validate_args(args)
        args.interval = 5.0
        with self.assertRaisesRegex(soak_monitor.SoakError, "twice"):
            soak_monitor.validate_args(args)
        args.interval = 10.0
        args.duration = float("nan")
        with self.assertRaisesRegex(soak_monitor.SoakError, "finite"):
            soak_monitor.validate_args(args)

    def test_summary_marks_alerts_as_failure(self) -> None:
        stats = soak_monitor.MonitorStats(60.0, "2026-01-01T00:00:00Z")
        status = healthy_status()
        stats.record_success(status, 1.0, 2.0, 3.0, ["relay-outputs-mask:1"])
        summary = stats.summary(
            finished_at="2026-01-01T00:01:00Z",
            elapsed_seconds=60.0,
            interrupted=False,
        )
        self.assertFalse(summary["success"])
        self.assertEqual(summary["samples_with_alerts"], 1)


if __name__ == "__main__":
    unittest.main()
