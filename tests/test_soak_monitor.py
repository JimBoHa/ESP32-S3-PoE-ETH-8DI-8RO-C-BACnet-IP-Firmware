#!/usr/bin/env python3
"""Host tests for the long-running health monitor."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

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
        "bacnet_udp_receive_mailbox_size": 64,
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


def instance_health_values(
    *, automatic: bool = True, conflicts: int = 0
) -> tuple[dict[str, object], dict[str, object]]:
    status = healthy_status()
    status.update(
        firmware_version="1.1.0",
        bacnet_instance_status="locked",
        bacnet_instance_conflicts=conflicts,
    )
    config = healthy_config()
    config.update(device_instance_auto=automatic, device_instance_locked=True)
    return status, config


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

    def test_baseline_rejects_undersized_udp_receive_mailbox(self) -> None:
        status = healthy_status()
        status["bacnet_udp_receive_mailbox_size"] = 63
        with self.assertRaisesRegex(soak_monitor.SoakError, "below the release minimum"):
            soak_monitor.Baseline.from_values(status, healthy_config())

    def test_locked_automatic_and_manual_instances_are_healthy(self) -> None:
        for automatic in (True, False):
            # Startup may resolve a collision before choosing and locking an ID.
            for conflicts in (0, 2):
                with self.subTest(automatic=automatic, conflicts=conflicts):
                    status, config = instance_health_values(
                        automatic=automatic, conflicts=conflicts
                    )
                    baseline = soak_monitor.Baseline.from_values(status, config)
                    self.assertEqual(baseline.bacnet_instance_conflicts, conflicts)
                    self.assertEqual(
                        soak_monitor.evaluate_sample(
                            baseline, status, status, config, bacnet_response(),
                            expected_relay_mask=0, minimum_heap_bytes=200000,
                        ),
                        [],
                    )

    def test_instance_health_rejects_unready_or_conflicting_baseline(self) -> None:
        for state in (
            "waiting-for-network", "discovering", "checking-candidate", "saving",
            "range-full", "save-failed", "reboot-required", "locked-conflict",
            "unknown", None,
        ):
            with self.subTest(state=state):
                status, config = instance_health_values()
                status["bacnet_instance_status"] = state
                with self.assertRaisesRegex(soak_monitor.SoakError, "instance"):
                    soak_monitor.Baseline.from_values(status, config)

    def test_instance_health_rejects_missing_or_invalid_telemetry(self) -> None:
        status_fields = {
            "bacnet_instance_status": (None, 1, True),
            "bacnet_instance_conflicts": (None, -1, 2**32, 0.0, True, "0"),
        }
        config_fields = {
            "device_instance_auto": (None, 0, "true"),
            "device_instance_locked": (None, False, 1, "true"),
        }
        for is_config, cases in ((False, status_fields), (True, config_fields)):
            for key, values in cases.items():
                for value in values:
                    with self.subTest(is_config=is_config, key=key, value=value):
                        status, config = instance_health_values()
                        target = config if is_config else status
                        target[key] = value
                        with self.assertRaisesRegex(soak_monitor.SoakError, "instance"):
                            soak_monitor.Baseline.from_values(status, config)
                with self.subTest(is_config=is_config, missing=key):
                    status, config = instance_health_values()
                    del (config if is_config else status)[key]
                    with self.assertRaisesRegex(soak_monitor.SoakError, "instance"):
                        soak_monitor.Baseline.from_values(status, config)

    def test_new_firmware_requires_instance_telemetry_even_if_all_fields_missing(self) -> None:
        for version in ("1.1.0", "1.1.0-dev", "1.2.0", "2.0.0"):
            with self.subTest(version=version):
                status = healthy_status()
                status["firmware_version"] = version
                with self.assertRaisesRegex(soak_monitor.SoakError, "instance"):
                    soak_monitor.Baseline.from_values(status, healthy_config())

    def test_partial_instance_telemetry_is_not_treated_as_legacy(self) -> None:
        for key, value in (
            ("bacnet_instance_status", "locked"),
            ("bacnet_instance_conflicts", 0),
        ):
            with self.subTest(key=key):
                status = healthy_status()
                status[key] = value
                with self.assertRaisesRegex(soak_monitor.SoakError, "instance"):
                    soak_monitor.Baseline.from_values(status, healthy_config())

    def test_instance_health_changes_alert_during_run(self) -> None:
        status, config = instance_health_values(conflicts=2)
        baseline = soak_monitor.Baseline.from_values(status, config)
        changes = (
            ({"bacnet_instance_status": "locked-conflict"}, {}, "instance-not-locked"),
            ({"bacnet_instance_status": "discovering"}, {}, "instance-not-locked"),
            ({"bacnet_instance_conflicts": 3}, {}, "bacnet_instance_conflicts-changed"),
            ({"bacnet_instance_conflicts": 0}, {}, "bacnet_instance_conflicts-changed"),
            ({"bacnet_instance_conflicts": True}, {}, "instance-conflicts-invalid"),
            ({}, {"device_instance_locked": False}, "instance-configuration-unlocked"),
            ({}, {"device_instance_auto": "true"}, "instance-mode-invalid"),
        )
        for status_delta, config_delta, expected in changes:
            with self.subTest(status=status_delta, config=config_delta):
                alerts = soak_monitor.evaluate_sample(
                    baseline, status, dict(status, **status_delta),
                    dict(config, **config_delta), bacnet_response(),
                    expected_relay_mask=0, minimum_heap_bytes=200000,
                )
                self.assertIn(expected, " ".join(alerts))
        missing = dict(status)
        del missing["bacnet_instance_status"]
        del missing["bacnet_instance_conflicts"]
        self.assertIn(
            "instance-not-locked",
            " ".join(soak_monitor.evaluate_sample(
                baseline, status, missing, config, bacnet_response(),
                expected_relay_mask=0, minimum_heap_bytes=200000,
            )),
        )

    def test_legacy_baseline_does_not_require_new_instance_fields(self) -> None:
        status = healthy_status()
        config = healthy_config()
        baseline = soak_monitor.Baseline.from_values(status, config)
        self.assertIsNone(baseline.bacnet_instance_conflicts)
        self.assertEqual(
            soak_monitor.evaluate_sample(
                baseline, status, status, config, bacnet_response(),
                expected_relay_mask=0, minimum_heap_bytes=200000,
            ),
            [],
        )

    def test_run_summary_fails_on_duplicate_without_actuating_device(self) -> None:
        for conflict in (False, True):
            with self.subTest(conflict=conflict), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "soak.jsonl"
                args = soak_monitor.build_parser().parse_args([
                    "--device-address", "192.168.75.154",
                    "--device-instance", "599153", "--output", str(path),
                    "--duration", "1", "--interval", "1", "--timeout", "0.1",
                ])
                status, config = instance_health_values()
                changed = dict(status)
                if conflict:
                    changed.update(
                        bacnet_instance_status="locked-conflict", bacnet_instance_conflicts=1
                    )
                bacnet = dict(bacnet_response(), latency_ms=1.0)
                responses = [
                    (status, config, bacnet, 1.0, 1.0),
                    (changed, config, bacnet, 1.0, 1.0),
                ]
                with (
                    patch.object(soak_monitor, "take_sample", side_effect=responses),
                    patch.object(soak_monitor.time, "monotonic", side_effect=range(100)),
                    patch("builtins.print"),
                ):
                    result = soak_monitor.run_monitor(args)
                records = [json.loads(line) for line in path.read_text().splitlines()]
                summary = records[-1]
                self.assertEqual(result, int(conflict))
                self.assertEqual(summary["samples"], 2)
                self.assertEqual(summary["request_failures"], 0)
                self.assertEqual(summary["samples_with_alerts"], int(conflict))
                self.assertEqual(summary["success"], not conflict)
                if conflict:
                    self.assertIn("bacnet-instance-not-locked", " ".join(records[-2]["alerts"]))

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
                "bacnet_udp_receive_mailbox_size": 65,
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
            "bacnet_udp_receive_mailbox_size-changed",
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
