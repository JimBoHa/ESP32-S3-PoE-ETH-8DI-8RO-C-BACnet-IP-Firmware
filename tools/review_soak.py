#!/usr/bin/env python3
"""Re-evaluate a saved soak log offline with the current health checks."""

from __future__ import annotations

import argparse
from dataclasses import replace
import hashlib
from itertools import islice
import json
import math
import os
from pathlib import Path
import sys
from typing import Any

if __package__:
    from . import soak_monitor as monitor
else:
    import soak_monitor as monitor


MAX_REVIEW_SAMPLES = 1_000_000


def finite_number(value: Any, name: str, *, positive: bool = False) -> float:
    if type(value) not in (int, float) or not math.isfinite(value):
        raise monitor.SoakError(f"{name} must be a finite number")
    if value < 0 or (positive and value == 0):
        raise monitor.SoakError(f"{name} is out of range")
    return float(value)


def reject_nonfinite_json(value: str) -> None:
    raise monitor.SoakError(f"non-finite JSON number: {value}")


def review_records(
    records: list[dict[str, Any]], *, minimum_duration: float | None = None
) -> dict[str, Any]:
    """Preserve recorded failures and independently verify completion and health."""
    origin = None
    summary = None
    samples: list[dict[str, Any]] = []
    for row in records:
        if not isinstance(row, dict):
            raise monitor.SoakError("each JSONL record must be an object")
        if summary is not None:
            raise monitor.SoakError("records found after the final summary")
        kind = row.get("type")
        if kind == "baseline":
            if origin is not None:
                raise monitor.SoakError("multiple baselines in one run")
            origin = row
        elif kind == "sample":
            if type(row.get("sequence")) is not int or row["sequence"] != len(samples):
                raise monitor.SoakError("sample sequence must start at zero with no gaps")
            if type(row.get("ok")) is not bool:
                raise monitor.SoakError("sample ok flag must be boolean")
            alerts = row.get("alerts")
            if not isinstance(alerts, list) or any(not isinstance(a, str) for a in alerts):
                raise monitor.SoakError("sample alerts must be a list of strings")
            if "error" not in row and origin is None:
                raise monitor.SoakError("response sample appears before its baseline")
            samples.append(row)
        elif kind == "summary":
            summary = row
        else:
            raise monitor.SoakError(f"unknown record type: {kind!r}")

    issues: list[str] = []
    findings: list[dict[str, Any]] = []
    baseline = None
    config = None
    options = None
    duration = None
    interval = None
    expected_samples = None
    schedule = None
    if minimum_duration is not None:
        minimum_duration = finite_number(minimum_duration, "required duration", positive=True)
    if origin is not None:
        for key in ("configuration", "monitor", "baseline", "target"):
            if not isinstance(origin.get(key), dict):
                raise monitor.SoakError(f"baseline {key} must be an object")
        config = origin["configuration"]
        options = origin["monitor"]
        saved = origin["baseline"]
        baseline = monitor.Baseline(**saved)
        duration = finite_number(options["duration_seconds"], "duration", positive=True)
        interval = finite_number(options["interval_seconds"], "interval", positive=True)
        # Use the recorder's exact floating-point schedule, including its final
        # boundary sample. Bound work even for nonsensically large input plans.
        expected_samples = sum(1 for _ in islice(
            monitor.sample_schedule(duration, interval), MAX_REVIEW_SAMPLES + 1
        ))
        if expected_samples > MAX_REVIEW_SAMPLES:
            raise monitor.SoakError(f"review supports at most {MAX_REVIEW_SAMPLES} scheduled samples")
        schedule = monitor.sample_schedule(duration, interval)
        if minimum_duration is not None and duration < minimum_duration:
            issues.append("planned-duration-below-required-minimum")
        for key, maximum in (("expected_relay_mask", 255), ("minimum_heap_bytes", None)):
            value = options[key]
            if type(value) is not int or value < 0 or (maximum is not None and value > maximum):
                raise monitor.SoakError(f"invalid {key}")
        if monitor.config_fingerprint(config) != baseline.config_sha256:
            issues.append("baseline-configuration-fingerprint-mismatch")
        target = origin["target"]
        if (
            target["device_instance"] != baseline.device_instance
            or target["device_address"] != baseline.ip_address
            or target["bacnet_port"] != baseline.bacnet_port
        ):
            issues.append("baseline-target-mismatch")
    else:
        issues.append("no-baseline")

    previous_status = None
    previous_elapsed = 0.0
    first_response = True
    reevaluated = 0
    request_failures = 0
    recorded_alert_samples = 0
    for row in samples:
        sequence = row["sequence"]
        elapsed = finite_number(row["elapsed_seconds"], "sample elapsed time")
        alerts = list(row.get("alerts", []))
        if row["ok"] is not True:
            alerts.append("recorded-sample-failed")
        if elapsed < previous_elapsed:
            alerts.append("sample-time-decreased")
        previous_elapsed = elapsed
        if schedule is not None:
            scheduled_offset = next(schedule, None)
            if scheduled_offset is not None and elapsed + 0.001 < scheduled_offset:
                alerts.append("sample-before-scheduled-time")
        if "error" in row:
            request_failures += 1
            alerts.append(f"recorded-request-failure:{row['error']}")
        else:
            recorded_alert_samples += bool(row.get("alerts"))
            status = row["status"]
            bacnet = row["bacnet"]
            if not isinstance(status, dict) or not isinstance(bacnet, dict):
                raise monitor.SoakError("response sample needs status and BACnet objects")
            if baseline is None or config is None or options is None or origin is None:
                raise monitor.SoakError("cannot evaluate a response without a baseline")
            if first_response:
                first_response = False
                if row["timestamp"] != origin["timestamp"]:
                    issues.append("baseline-not-from-first-response")
                try:
                    candidate = monitor.Baseline.from_values(status, config)
                except (monitor.SoakError, TypeError, ValueError) as error:
                    alerts.append(f"baseline-health:{error}")
                else:
                    # Older recorders logged the new status fields but did not
                    # persist their counter in Baseline. Recover it from the
                    # same response, without replacing any original baseline.
                    if "bacnet_instance_conflicts" not in origin["baseline"]:
                        baseline = replace(
                            baseline, bacnet_instance_conflicts=candidate.bacnet_instance_conflicts
                        )
                    if candidate != baseline:
                        issues.append("saved-baseline-does-not-match-first-response")
            if row.get("config_sha256") != baseline.config_sha256:
                alerts.append("configuration-content-changed-or-unavailable")
            if row.get("config_database_revision") != baseline.config_database_revision:
                alerts.append("configuration-database-revision-changed-or-unavailable")
            alerts.extend(monitor.evaluate_sample(
                baseline, previous_status, status, config, bacnet,
                expected_relay_mask=options["expected_relay_mask"],
                minimum_heap_bytes=options["minimum_heap_bytes"],
            ))
            previous_status = status
            reevaluated += 1
        if alerts:
            findings.append({"sequence": sequence, "alerts": list(dict.fromkeys(alerts))})

    if not samples:
        issues.append("no-samples")
    completed_duration = False
    if expected_samples is not None and len(samples) > expected_samples:
        issues.append("more-samples-than-scheduled")
    if summary is not None:
        for key, actual in (
            ("samples", len(samples)),
            ("successful_samples", len(samples) - request_failures),
            ("request_failures", request_failures),
            ("samples_with_alerts", recorded_alert_samples),
        ):
            if type(summary.get(key)) is not int or summary[key] != actual:
                issues.append(f"summary-{key}-mismatch")
        if summary.get("success") is not True:
            issues.append("recorded-summary-not-successful")
        if summary.get("interrupted") is not False:
            issues.append("recorded-run-interrupted-or-unknown")
        summary_elapsed = finite_number(summary["elapsed_seconds"], "summary elapsed time")
        if summary_elapsed + 0.001 < previous_elapsed:
            issues.append("summary-precedes-last-sample")
        if duration is not None:
            if summary.get("planned_duration_seconds") != duration:
                issues.append("summary-planned-duration-mismatch")
            completed_duration = (
                summary.get("interrupted") is False
                and summary_elapsed + 0.001 >= duration
                and previous_elapsed + 0.001 >= duration
                and len(samples) == expected_samples
            )
        if not completed_duration:
            issues.append("full-scheduled-duration-not-proven")

    result = "fail" if issues or findings else "pass" if completed_duration else "incomplete"
    return {
        "type": "offline-soak-review",
        "result": result,
        "success": result == "pass",
        "completed_duration": completed_duration,
        "samples": len(samples),
        "expected_samples": expected_samples,
        "required_duration_seconds": minimum_duration,
        "reevaluated_samples": reevaluated,
        "request_failures": request_failures,
        "samples_with_findings": len(findings),
        "last_sample_timestamp": samples[-1].get("timestamp") if samples else None,
        "elapsed_seconds": previous_elapsed,
        "baseline_instance_conflicts": baseline.bacnet_instance_conflicts if baseline else None,
        "issues": issues,
        "sample_findings": findings,
        "recorded_summary": summary,
    }


def review_file(path: Path, *, minimum_duration: float | None = None) -> dict[str, Any]:
    # Capture the length before reading: a running logger may continue appending.
    with path.open("rb") as stream:
        payload = stream.read(os.fstat(stream.fileno()).st_size)
    if not payload or not payload.endswith(b"\n"):
        raise monitor.SoakError("empty or partial JSONL snapshot; retry after the writer flushes")
    records = [json.loads(line, parse_constant=reject_nonfinite_json) for line in payload.splitlines()]
    result = review_records(records, minimum_duration=minimum_duration)
    result.update(
        log_sha256=hashlib.sha256(payload).hexdigest(),
        log_bytes=len(payload),
        reviewer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        health_checks_sha256=hashlib.sha256(Path(monitor.__file__).read_bytes()).hexdigest(),
    )
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="existing JSONL log; never modified")
    parser.add_argument("--minimum-duration", type=float, help="minimum planned run length in seconds")
    args = parser.parse_args(argv)
    try:
        result = review_file(args.log, minimum_duration=args.minimum_duration)
    except (OSError, UnicodeError, ValueError, TypeError, KeyError, OverflowError, monitor.SoakError) as error:
        print(f"error: cannot review soak log: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, sort_keys=True))
    return {"pass": 0, "fail": 1, "incomplete": 2}[result["result"]]


if __name__ == "__main__":
    raise SystemExit(main())
