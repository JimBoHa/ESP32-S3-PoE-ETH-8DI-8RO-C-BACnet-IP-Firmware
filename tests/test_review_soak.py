#!/usr/bin/env python3
"""Offline replay must not turn incomplete or failed evidence into a pass."""

from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools import review_soak, soak_monitor


def healthy_log(*, modern: bool = True, old_recorder: bool = False) -> list[dict]:
    status = {
        "firmware_version": "1.1.0" if modern else "0.13.5",
        "build_date": "Sep  9 2026", "running_partition": "ota_0",
        "reboot_count": 47, "last_reset_reason": "software", "last_reset_reason_code": 3,
        "product": "ESP32 BACnet I/O", "ip_address": "192.0.2.10",
        "bacnet_device_instance": 599153, "bacnet_vendor_id": 260,
        "bacnet_udp_port": 47808, "bacnet_udp_receive_mailbox_size": 64,
        "ethernet_link": True, "ipv4_assigned": True, "bacnet_running": True,
        "relay_controller_healthy": True, "rtc_present": True,
        "relay_outputs_mask": 0, "relay_commands_mask": 0, "relay_active_priorities": [0] * 8,
        "free_heap_bytes": 259356, "minimum_free_heap_bytes": 254740,
        "uptime_seconds": 100, "bacnet_packets_received": 50,
    }
    config = {"database_revision": 16, "device_instance": 599153,
              "vendor_id": 260, "bacnet_port": 47808}
    if modern:
        status.update(bacnet_instance_status="locked", bacnet_instance_conflicts=0)
        config.update(device_instance_auto=True, device_instance_locked=True)
    baseline = soak_monitor.Baseline.from_values(status, config).__dict__.copy()
    if old_recorder:
        del baseline["bacnet_instance_conflicts"]
    start = "2026-09-09T00:00:00Z"
    records = [{
        "type": "baseline", "timestamp": start,
        "target": {"device_address": "192.0.2.10", "device_instance": 599153, "bacnet_port": 47808},
        "monitor": {"duration_seconds": 120, "interval_seconds": 60,
                    "timeout_seconds": 5, "minimum_heap_bytes": 250000, "expected_relay_mask": 0},
        "baseline": baseline, "configuration": config,
    }]
    stats = soak_monitor.MonitorStats(120, start)
    for sequence in range(3):
        current = dict(status, uptime_seconds=100 + 60 * sequence, bacnet_packets_received=50 + sequence)
        records.append({
            "type": "sample", "sequence": sequence, "timestamp": f"2026-09-09T00:0{sequence}:00Z",
            "elapsed_seconds": 60 * sequence + 0.03, "ok": True, "alerts": [],
            "status": current, "config_sha256": soak_monitor.config_fingerprint(config),
            "config_database_revision": 16,
            "bacnet": {"device_instance": 599153, "vendor_id": 260,
                       "max_apdu": 1476, "segmentation": 3, "latency_ms": 4.0},
            "http_status_latency_ms": 12.0, "http_config_latency_ms": 13.0,
        })
        stats.record_success(current, 12.0, 13.0, 4.0, [])
    records.append(stats.summary(
        finished_at="2026-09-09T00:02:00Z", elapsed_seconds=120.04, interrupted=False
    ))
    return records


class ReviewSoakTests(unittest.TestCase):
    def test_current_and_legacy_recorders_and_firmware(self) -> None:
        for modern in (False, True):
            for old_recorder in (False, True):
                with self.subTest(modern=modern, old_recorder=old_recorder):
                    result = review_soak.review_records(healthy_log(modern=modern, old_recorder=old_recorder))
                    self.assertEqual(result["result"], "pass")
                    self.assertTrue(result["success"])
                    self.assertTrue(result["completed_duration"])
                    self.assertEqual(result["reevaluated_samples"], 3)
                    self.assertEqual(result["baseline_instance_conflicts"], 0 if modern else None)

    def test_old_success_flags_do_not_hide_new_health_faults(self) -> None:
        for change in (
            {"bacnet_instance_status": "locked-conflict"},
            {"bacnet_instance_conflicts": 1},
            {"bacnet_instance_conflicts": -1},
            {"bacnet_instance_status": None, "bacnet_instance_conflicts": None},
            {"reboot_count": 48}, {"relay_outputs_mask": 1},
            {"free_heap_bytes": 100}, {"ethernet_link": False},
        ):
            with self.subTest(change=change):
                records = healthy_log(old_recorder=True)
                records[2]["status"].update(change)
                result = review_soak.review_records(records)
                self.assertEqual(result["result"], "fail")
                self.assertGreater(result["samples_with_findings"], 0)
                self.assertFalse(result["success"])

    def test_recovered_counter_does_not_replace_original_baseline(self) -> None:
        records = healthy_log(old_recorder=True)
        records[1]["status"]["reboot_count"] += 1
        result = review_soak.review_records(records)
        self.assertIn("saved-baseline-does-not-match-first-response", result["issues"])
        self.assertIn("reboot_count-changed", json.dumps(result["sample_findings"]))

    def test_existing_counter_is_never_rebased(self) -> None:
        records = healthy_log()
        records[0]["baseline"]["bacnet_instance_conflicts"] = 2
        result = review_soak.review_records(records)
        self.assertEqual(result["baseline_instance_conflicts"], 2)
        self.assertEqual(result["result"], "fail")

    def test_unhealthy_first_response_cannot_become_a_baseline(self) -> None:
        records = healthy_log(old_recorder=True)
        records[1]["status"]["bacnet_instance_status"] = "locked-conflict"
        result = review_soak.review_records(records)
        self.assertEqual(result["result"], "fail")
        self.assertIn("baseline-health", json.dumps(result["sample_findings"]))

    def test_resolved_initial_conflicts_are_allowed_but_changes_fail(self) -> None:
        records = healthy_log(old_recorder=True)
        for row in records[1:-1]:
            row["status"]["bacnet_instance_conflicts"] = 2
        self.assertEqual(review_soak.review_records(records)["result"], "pass")
        records[2]["status"]["bacnet_instance_conflicts"] = 0
        self.assertEqual(review_soak.review_records(records)["result"], "fail")

    def test_original_alerts_and_false_flags_are_preserved(self) -> None:
        for change in ({"alerts": ["old-validator-only-alert"]}, {"ok": False}):
            with self.subTest(change=change):
                records = healthy_log()
                records[2].update(change)
                result = review_soak.review_records(records)
                self.assertEqual(result["result"], "fail")
                self.assertEqual(result["samples_with_findings"], 1)

    def test_request_failure_is_preserved_and_later_samples_rechecked(self) -> None:
        records = healthy_log()
        records[2] = {key: records[2][key] for key in ("type", "sequence", "timestamp", "elapsed_seconds")}
        records[2].update(ok=False, alerts=["request-failure:SoakError"], error="I-Am timed out")
        records[-1].update(success=False, request_failures=1, successful_samples=2)
        result = review_soak.review_records(records)
        self.assertEqual(result["result"], "fail")
        self.assertEqual(result["request_failures"], 1)
        self.assertEqual(result["reevaluated_samples"], 2)
        self.assertIn("I-Am timed out", json.dumps(result["sample_findings"]))

    def test_failed_request_before_baseline_is_not_discarded(self) -> None:
        records = healthy_log()
        failure = {"type": "sample", "sequence": 0, "timestamp": "earlier",
                   "elapsed_seconds": 0.03, "ok": False, "alerts": [], "error": "timeout"}
        for row in records[1:-1]:
            row["sequence"] += 1
            row["elapsed_seconds"] += 60
        records.insert(0, failure)
        result = review_soak.review_records(records)
        self.assertEqual(result["result"], "fail")
        self.assertEqual(result["request_failures"], 1)
        self.assertEqual(result["reevaluated_samples"], 3)

    def test_clean_live_snapshot_is_incomplete_even_after_last_sample(self) -> None:
        for count in (1, 2, 3):
            with self.subTest(samples=count):
                result = review_soak.review_records(healthy_log()[:count + 1])
                self.assertEqual(result["result"], "incomplete")
                self.assertFalse(result["success"])
                self.assertFalse(result["completed_duration"])

    def test_summary_alone_cannot_prove_a_full_run(self) -> None:
        changes = (
            ("elapsed_seconds", 119), ("samples", 2), ("samples", True),
            ("successful_samples", 2), ("request_failures", 1),
            ("samples_with_alerts", 1), ("planned_duration_seconds", 60),
            ("interrupted", True), ("success", False),
        )
        for key, value in changes:
            with self.subTest(key=key, value=value):
                records = healthy_log()
                records[-1][key] = value
                self.assertEqual(review_soak.review_records(records)["result"], "fail")
        records = healthy_log()
        del records[-2]
        self.assertEqual(review_soak.review_records(records)["result"], "fail")

    def test_configuration_and_baseline_evidence_checked(self) -> None:
        for key, value in (("config_sha256", "different"), ("config_database_revision", 17)):
            with self.subTest(key=key):
                records = healthy_log()
                records[2][key] = value
                self.assertEqual(review_soak.review_records(records)["result"], "fail")
        for mutate in (
            lambda row: row["target"].update(device_instance=1),
            lambda row: row["configuration"].update(device_name="changed"),
            lambda row: row.update(timestamp="different response"),
        ):
            records = healthy_log()
            mutate(records[0])
            self.assertEqual(review_soak.review_records(records)["result"], "fail")

    def test_missing_duplicate_and_reordered_rows_rejected(self) -> None:
        good = healthy_log()
        cases = [good[:2] + good[3:], good + [good[-1]], good[:1] + good,
                 [good[1], good[0]] + good[2:], good[:2] + [{"type": "unexpected"}] + good[2:]]
        for records in cases:
            with self.subTest(records=records), self.assertRaises(soak_monitor.SoakError):
                review_soak.review_records(records)

    def test_bad_metadata_rejected(self) -> None:
        for key, value in (("duration_seconds", 0), ("interval_seconds", float("nan")),
                           ("expected_relay_mask", True), ("minimum_heap_bytes", -1)):
            with self.subTest(key=key):
                records = healthy_log()
                records[0]["monitor"][key] = value
                with self.assertRaises(soak_monitor.SoakError):
                    review_soak.review_records(records)
        for value in (None, [], "bad"):
            records = healthy_log()
            records[0]["configuration"] = value
            with self.assertRaises(soak_monitor.SoakError):
                review_soak.review_records(records)

    def test_elapsed_time_must_be_finite_monotonic_and_scheduled(self) -> None:
        for value in (-1, True, float("nan"), "60"):
            records = healthy_log()
            records[2]["elapsed_seconds"] = value
            with self.assertRaises(soak_monitor.SoakError):
                review_soak.review_records(records)
        for value in (0, 59):
            records = healthy_log()
            records[2]["elapsed_seconds"] = value
            self.assertEqual(review_soak.review_records(records)["result"], "fail")

    def test_file_cli_exit_codes_hashes_and_no_network_or_log_changes(self) -> None:
        for expected, change in ((0, "none"), (1, "fault"), (2, "live")):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "soak.jsonl"
                records = healthy_log()
                if change == "fault":
                    records[2]["status"]["bacnet_instance_conflicts"] = 1
                elif change == "live":
                    records.pop()
                payload = "".join(json.dumps(row) + "\n" for row in records).encode()
                path.write_bytes(payload)
                with patch.object(soak_monitor, "take_sample", side_effect=AssertionError("network")), \
                     patch.object(soak_monitor, "fetch_json", side_effect=AssertionError("network")), \
                     patch.object(soak_monitor, "probe_bacnet", side_effect=AssertionError("network")), \
                     patch("builtins.print"):
                    self.assertEqual(review_soak.main([str(path)]), expected)
                result = review_soak.review_file(path)
                self.assertEqual(result["log_sha256"], hashlib.sha256(payload).hexdigest())
                self.assertEqual(result["log_bytes"], len(payload))
                self.assertEqual(len(result["health_checks_sha256"]), 64)
                self.assertEqual(len(result["reviewer_sha256"]), 64)
                self.assertEqual(path.read_bytes(), payload)

    def test_malformed_and_partial_files_fail_closed(self) -> None:
        for payload in (b"", b"{", b"{}", b"null\n", b"[]\n", b"{bad}\n", b"{\"x\":NaN}\n"):
            with self.subTest(payload=payload), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "soak.jsonl"
                path.write_bytes(payload)
                with patch("builtins.print"):
                    self.assertEqual(review_soak.main([str(path)]), 2)
                self.assertEqual(path.read_bytes(), payload)

    def test_empty_and_failed_only_runs_never_pass(self) -> None:
        self.assertEqual(review_soak.review_records([])["result"], "fail")
        self.assertEqual(review_soak.review_records([{
            "type": "sample", "sequence": 0, "elapsed_seconds": 0.1,
            "ok": False, "alerts": [], "error": "timeout",
        }])["result"], "fail")

    def test_review_does_not_mutate_input_records(self) -> None:
        records = healthy_log(old_recorder=True)
        original = deepcopy(records)
        review_soak.review_records(records)
        self.assertEqual(records, original)

    def test_minimum_duration_prevents_short_preflight_from_passing_release_gate(self) -> None:
        for required in (60, 120, 86400):
            with self.subTest(required=required):
                result = review_soak.review_records(healthy_log(), minimum_duration=required)
                self.assertEqual(result["result"], "pass" if required <= 120 else "fail")
        with self.assertRaises(soak_monitor.SoakError):
            review_soak.review_records(healthy_log(), minimum_duration=0)

    def test_exact_fractional_schedule_and_work_limit(self) -> None:
        records = healthy_log()
        records[0]["monitor"].update(duration_seconds=1, interval_seconds=0.1)
        # Repeated floating-point addition produces a near-1.0 sample followed
        # by the explicit 1.0 boundary; ceil(duration / interval) loses one.
        samples = []
        for sequence, offset in enumerate(soak_monitor.sample_schedule(1, 0.1)):
            row = deepcopy(records[1])
            row.update(sequence=sequence, elapsed_seconds=offset + 0.03)
            samples.append(row)
        records = [records[0], *samples, records[-1]]
        records[-1].update(samples=len(samples), successful_samples=len(samples),
                           planned_duration_seconds=1, elapsed_seconds=1.04)
        self.assertEqual(review_soak.review_records(records)["result"], "pass")
        with patch.object(review_soak, "MAX_REVIEW_SAMPLES", 2):
            with self.assertRaisesRegex(soak_monitor.SoakError, "at most"):
                review_soak.review_records(healthy_log())


if __name__ == "__main__":
    unittest.main()
