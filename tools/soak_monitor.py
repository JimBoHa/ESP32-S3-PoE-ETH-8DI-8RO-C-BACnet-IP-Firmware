#!/usr/bin/env python3
"""Continuously monitor controller health over HTTP and BACnet/IP."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timezone
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import socket
import statistics
import sys
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import ProxyHandler, Request, build_opener


BACNET_PORT = 47808
WHO_IS_UNICAST = bytes.fromhex("81 0a 00 08 01 00 10 08")
MAX_HTTP_RESPONSE_SIZE = 1024 * 1024
RELAY_COUNT = 8
DIRECT_OPENER = build_opener(ProxyHandler({}))


class SoakError(RuntimeError):
    """Expected monitor or device error."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def config_fingerprint(config: dict[str, Any]) -> str:
    canonical = json.dumps(
        config, separators=(",", ":"), sort_keys=True, ensure_ascii=True
    ).encode("ascii")
    return hashlib.sha256(canonical).hexdigest()


def fetch_json(device_address: str, path: str, timeout: float) -> tuple[dict[str, Any], float]:
    request = Request(
        f"http://{device_address}{path}",
        headers={"Accept": "application/json", "User-Agent": "bacnet-io-soak/1"},
    )
    started = time.monotonic()
    try:
        with DIRECT_OPENER.open(request, timeout=timeout) as response:
            payload = response.read(MAX_HTTP_RESPONSE_SIZE + 1)
    except HTTPError as error:
        raise SoakError(f"{path} returned HTTP {error.code}") from error
    except (URLError, TimeoutError, OSError) as error:
        raise SoakError(f"{path} request failed: {error}") from error
    elapsed_ms = (time.monotonic() - started) * 1000.0
    if len(payload) > MAX_HTTP_RESPONSE_SIZE:
        raise SoakError(f"{path} response exceeds 1 MiB")
    try:
        value = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SoakError(f"{path} returned invalid JSON") from error
    if not isinstance(value, dict):
        raise SoakError(f"{path} did not return a JSON object")
    return value, elapsed_ms


def decode_tag(payload: bytes, offset: int) -> tuple[int, bytes, int]:
    if offset >= len(payload):
        raise SoakError("truncated BACnet application tag")
    header = payload[offset]
    tag_number = header >> 4
    if tag_number == 15 or header & 0x08:
        raise SoakError("unexpected extended or context BACnet tag")
    length = header & 0x07
    offset += 1
    if length == 5:
        if offset >= len(payload):
            raise SoakError("truncated BACnet tag length")
        length = payload[offset]
        offset += 1
        if length == 254:
            if offset + 2 > len(payload):
                raise SoakError("truncated BACnet 16-bit tag length")
            length = int.from_bytes(payload[offset : offset + 2], "big")
            offset += 2
        elif length == 255:
            if offset + 4 > len(payload):
                raise SoakError("truncated BACnet 32-bit tag length")
            length = int.from_bytes(payload[offset : offset + 4], "big")
            offset += 4
    end = offset + length
    if end > len(payload):
        raise SoakError("BACnet tag value exceeds packet")
    return tag_number, payload[offset:end], end


def parse_i_am(packet: bytes) -> dict[str, int]:
    if len(packet) < 12 or packet[0] != 0x81:
        raise SoakError("not a BACnet/IPv4 packet")
    function = packet[1]
    if function not in {0x0A, 0x0B}:
        raise SoakError(f"unexpected BVLC function 0x{function:02x}")
    declared_length = int.from_bytes(packet[2:4], "big")
    if declared_length != len(packet):
        raise SoakError(
            f"BACnet packet length mismatch ({declared_length} != {len(packet)})"
        )
    offset = 4
    if offset + 2 > len(packet):
        raise SoakError("truncated BACnet NPDU")
    if packet[offset] != 1:
        raise SoakError("unsupported BACnet NPDU version")
    control = packet[offset + 1]
    offset += 2
    if control & 0x20:
        if offset + 3 > len(packet):
            raise SoakError("truncated NPDU destination")
        destination_length = packet[offset + 2]
        offset += 3 + destination_length + 1
        if offset > len(packet):
            raise SoakError("truncated NPDU destination address")
    if control & 0x08:
        if offset + 3 > len(packet):
            raise SoakError("truncated NPDU source")
        source_length = packet[offset + 2]
        offset += 3 + source_length
        if offset > len(packet):
            raise SoakError("truncated NPDU source address")
    if control & 0x80:
        raise SoakError("received a network-layer message instead of I-Am")
    if offset + 2 > len(packet) or packet[offset] >> 4 != 1 or packet[offset + 1] != 0:
        raise SoakError("BACnet APDU is not an I-Am")
    offset += 2

    values: list[tuple[int, bytes]] = []
    while offset < len(packet):
        tag, value, offset = decode_tag(packet, offset)
        values.append((tag, value))
    expected_tags = [12, 2, 9, 2]
    if [tag for tag, _value in values] != expected_tags:
        raise SoakError("I-Am has unexpected application tags")
    if len(values[0][1]) != 4:
        raise SoakError("I-Am Device identifier is not four bytes")
    object_identifier = int.from_bytes(values[0][1], "big")
    object_type = object_identifier >> 22
    if object_type != 8:
        raise SoakError(f"I-Am object type is {object_type}, not Device")
    return {
        "device_instance": object_identifier & 0x3FFFFF,
        "max_apdu": int.from_bytes(values[1][1], "big"),
        "segmentation": int.from_bytes(values[2][1], "big"),
        "vendor_id": int.from_bytes(values[3][1], "big"),
        "bvlc_function": function,
    }


def probe_bacnet(
    device_address: str, device_instance: int, port: int, timeout: float
) -> dict[str, Any]:
    started = time.monotonic()
    deadline = started + timeout
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(timeout)
            client.sendto(WHO_IS_UNICAST, (device_address, port))
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise SoakError("BACnet I-Am timed out")
                client.settimeout(remaining)
                packet, source = client.recvfrom(2048)
                response = parse_i_am(packet)
                if response["device_instance"] != device_instance:
                    continue
                if source[0] != device_address or source[1] != port:
                    raise SoakError(f"I-Am came from unexpected source {source}")
                response["source"] = f"{source[0]}:{source[1]}"
                response["latency_ms"] = round((time.monotonic() - started) * 1000.0, 3)
                return response
    except socket.timeout as error:
        raise SoakError("BACnet I-Am timed out") from error
    except OSError as error:
        raise SoakError(f"BACnet probe failed: {error}") from error


@dataclass
class Baseline:
    firmware_version: str
    build_date: str
    running_partition: str
    reboot_count: int
    reset_reason: str
    reset_reason_code: int
    product: str
    ip_address: str
    device_instance: int
    vendor_id: int
    bacnet_port: int
    config_database_revision: int
    config_sha256: str

    @classmethod
    def from_values(
        cls, status: dict[str, Any], config: dict[str, Any]
    ) -> "Baseline":
        required_status = (
            "firmware_version",
            "build_date",
            "running_partition",
            "reboot_count",
            "last_reset_reason",
            "last_reset_reason_code",
            "product",
            "ip_address",
            "bacnet_device_instance",
            "bacnet_vendor_id",
            "bacnet_udp_port",
        )
        missing = [key for key in required_status if key not in status]
        if missing:
            raise SoakError(f"status is missing required fields: {', '.join(missing)}")
        required_config = (
            "database_revision",
            "device_instance",
            "vendor_id",
            "bacnet_port",
        )
        missing = [key for key in required_config if key not in config]
        if missing:
            raise SoakError(
                f"configuration is missing required fields: {', '.join(missing)}"
            )
        if int(config["device_instance"]) != int(status["bacnet_device_instance"]):
            raise SoakError("HTTP status and configuration Device instances disagree")
        if int(config["vendor_id"]) != int(status["bacnet_vendor_id"]):
            raise SoakError("HTTP status and configuration vendor identifiers disagree")
        if int(config["bacnet_port"]) != int(status["bacnet_udp_port"]):
            raise SoakError("HTTP status and configuration BACnet ports disagree")
        return cls(
            firmware_version=str(status["firmware_version"]),
            build_date=str(status["build_date"]),
            running_partition=str(status["running_partition"]),
            reboot_count=int(status["reboot_count"]),
            reset_reason=str(status["last_reset_reason"]),
            reset_reason_code=int(status["last_reset_reason_code"]),
            product=str(status["product"]),
            ip_address=str(status["ip_address"]),
            device_instance=int(status["bacnet_device_instance"]),
            vendor_id=int(status["bacnet_vendor_id"]),
            bacnet_port=int(status["bacnet_udp_port"]),
            config_database_revision=int(config["database_revision"]),
            config_sha256=config_fingerprint(config),
        )


def evaluate_sample(
    baseline: Baseline,
    previous_status: dict[str, Any] | None,
    status: dict[str, Any],
    config: dict[str, Any],
    bacnet: dict[str, Any],
    *,
    expected_relay_mask: int,
    minimum_heap_bytes: int,
) -> list[str]:
    alerts: list[str] = []

    stable_fields = {
        "firmware_version": baseline.firmware_version,
        "build_date": baseline.build_date,
        "running_partition": baseline.running_partition,
        "reboot_count": baseline.reboot_count,
        "last_reset_reason": baseline.reset_reason,
        "last_reset_reason_code": baseline.reset_reason_code,
        "product": baseline.product,
        "ip_address": baseline.ip_address,
        "bacnet_device_instance": baseline.device_instance,
        "bacnet_vendor_id": baseline.vendor_id,
        "bacnet_udp_port": baseline.bacnet_port,
    }
    for key, expected in stable_fields.items():
        if status.get(key) != expected:
            alerts.append(f"{key}-changed:{status.get(key)!r}!={expected!r}")

    if config.get("database_revision") != baseline.config_database_revision:
        alerts.append(
            "database-revision-changed:"
            f"{config.get('database_revision')!r}!={baseline.config_database_revision}"
        )
    fingerprint = config_fingerprint(config)
    if fingerprint != baseline.config_sha256:
        alerts.append("configuration-content-changed")

    required_true = (
        "ethernet_link",
        "ipv4_assigned",
        "bacnet_running",
        "relay_controller_healthy",
        "rtc_present",
    )
    for key in required_true:
        if status.get(key) is not True:
            alerts.append(f"{key}-unhealthy:{status.get(key)!r}")

    expected_priorities = [0] * RELAY_COUNT if expected_relay_mask == 0 else None
    if status.get("relay_outputs_mask") != expected_relay_mask:
        alerts.append(
            f"relay-outputs-mask:{status.get('relay_outputs_mask')!r}!={expected_relay_mask}"
        )
    if status.get("relay_commands_mask") != expected_relay_mask:
        alerts.append(
            f"relay-commands-mask:{status.get('relay_commands_mask')!r}!={expected_relay_mask}"
        )
    priorities = status.get("relay_active_priorities")
    if not isinstance(priorities, list) or len(priorities) != RELAY_COUNT:
        alerts.append("relay-priorities-malformed")
    elif expected_priorities is not None and priorities != expected_priorities:
        alerts.append(f"relay-priorities-active:{priorities!r}")

    try:
        free_heap = int(status["free_heap_bytes"])
        reported_minimum = int(status["minimum_free_heap_bytes"])
    except (KeyError, TypeError, ValueError):
        alerts.append("heap-metrics-missing-or-invalid")
    else:
        if free_heap < minimum_heap_bytes:
            alerts.append(f"free-heap-below-floor:{free_heap}<{minimum_heap_bytes}")
        if reported_minimum < minimum_heap_bytes:
            alerts.append(
                f"minimum-heap-below-floor:{reported_minimum}<{minimum_heap_bytes}"
            )
        if reported_minimum > free_heap:
            alerts.append(f"minimum-heap-exceeds-current:{reported_minimum}>{free_heap}")

    if previous_status is not None:
        for key in ("uptime_seconds", "bacnet_packets_received"):
            try:
                current = int(status[key])
                previous = int(previous_status[key])
            except (KeyError, TypeError, ValueError):
                alerts.append(f"{key}-missing-or-invalid")
            else:
                if current < previous:
                    alerts.append(f"{key}-decreased:{current}<{previous}")

    if bacnet.get("device_instance") != baseline.device_instance:
        alerts.append(
            f"bacnet-device-instance:{bacnet.get('device_instance')!r}!={baseline.device_instance}"
        )
    if bacnet.get("vendor_id") != baseline.vendor_id:
        alerts.append(f"bacnet-vendor-id:{bacnet.get('vendor_id')!r}!={baseline.vendor_id}")
    if bacnet.get("max_apdu") != 1476 or bacnet.get("segmentation") != 3:
        alerts.append(
            f"bacnet-capabilities:apdu={bacnet.get('max_apdu')!r},"
            f"segmentation={bacnet.get('segmentation')!r}"
        )
    return alerts


@dataclass
class MonitorStats:
    planned_duration_seconds: float
    started_at: str
    samples: int = 0
    successful_samples: int = 0
    request_failures: int = 0
    samples_with_alerts: int = 0
    consecutive_failures: int = 0
    maximum_consecutive_failures: int = 0
    alert_counts: Counter[str] = field(default_factory=Counter)
    http_status_latencies: list[float] = field(default_factory=list)
    http_config_latencies: list[float] = field(default_factory=list)
    bacnet_latencies: list[float] = field(default_factory=list)
    first_free_heap: int | None = None
    last_free_heap: int | None = None
    lowest_free_heap: int | None = None
    lowest_reported_minimum_heap: int | None = None
    first_uptime: int | None = None
    last_uptime: int | None = None
    first_packet_count: int | None = None
    last_packet_count: int | None = None

    def record_success(
        self,
        status: dict[str, Any],
        http_status_ms: float,
        http_config_ms: float,
        bacnet_ms: float,
        alerts: list[str],
    ) -> None:
        self.samples += 1
        self.successful_samples += 1
        self.consecutive_failures = 0
        self.http_status_latencies.append(http_status_ms)
        self.http_config_latencies.append(http_config_ms)
        self.bacnet_latencies.append(bacnet_ms)
        free_heap = int(status["free_heap_bytes"])
        minimum_heap = int(status["minimum_free_heap_bytes"])
        uptime = int(status["uptime_seconds"])
        packets = int(status["bacnet_packets_received"])
        if self.first_free_heap is None:
            self.first_free_heap = free_heap
            self.first_uptime = uptime
            self.first_packet_count = packets
        self.last_free_heap = free_heap
        self.last_uptime = uptime
        self.last_packet_count = packets
        self.lowest_free_heap = (
            free_heap if self.lowest_free_heap is None else min(self.lowest_free_heap, free_heap)
        )
        self.lowest_reported_minimum_heap = (
            minimum_heap
            if self.lowest_reported_minimum_heap is None
            else min(self.lowest_reported_minimum_heap, minimum_heap)
        )
        if alerts:
            self.samples_with_alerts += 1
            self.alert_counts.update(alert.split(":", 1)[0] for alert in alerts)

    def record_failure(self, category: str) -> None:
        self.samples += 1
        self.request_failures += 1
        self.consecutive_failures += 1
        self.maximum_consecutive_failures = max(
            self.maximum_consecutive_failures, self.consecutive_failures
        )
        self.alert_counts[category] += 1

    @staticmethod
    def latency_summary(values: list[float]) -> dict[str, float] | None:
        if not values:
            return None
        ordered = sorted(values)
        index_95 = min(len(ordered) - 1, math.ceil(0.95 * len(ordered)) - 1)
        return {
            "minimum_ms": round(ordered[0], 3),
            "mean_ms": round(statistics.fmean(ordered), 3),
            "p95_ms": round(ordered[index_95], 3),
            "maximum_ms": round(ordered[-1], 3),
        }

    def summary(
        self, *, finished_at: str, elapsed_seconds: float, interrupted: bool
    ) -> dict[str, Any]:
        availability = (
            100.0 * self.successful_samples / self.samples if self.samples else 0.0
        )
        return {
            "type": "summary",
            "started_at": self.started_at,
            "finished_at": finished_at,
            "planned_duration_seconds": self.planned_duration_seconds,
            "elapsed_seconds": round(elapsed_seconds, 3),
            "interrupted": interrupted,
            "samples": self.samples,
            "successful_samples": self.successful_samples,
            "request_failures": self.request_failures,
            "samples_with_alerts": self.samples_with_alerts,
            "maximum_consecutive_failures": self.maximum_consecutive_failures,
            "availability_percent": round(availability, 6),
            "alert_counts": dict(sorted(self.alert_counts.items())),
            "free_heap": {
                "first_bytes": self.first_free_heap,
                "last_bytes": self.last_free_heap,
                "delta_bytes": (
                    self.last_free_heap - self.first_free_heap
                    if self.first_free_heap is not None and self.last_free_heap is not None
                    else None
                ),
                "lowest_current_bytes": self.lowest_free_heap,
                "lowest_reported_minimum_bytes": self.lowest_reported_minimum_heap,
            },
            "uptime": {
                "first_seconds": self.first_uptime,
                "last_seconds": self.last_uptime,
            },
            "bacnet_packets_received": {
                "first": self.first_packet_count,
                "last": self.last_packet_count,
                "delta": (
                    self.last_packet_count - self.first_packet_count
                    if self.first_packet_count is not None
                    and self.last_packet_count is not None
                    else None
                ),
            },
            "latency": {
                "http_status": self.latency_summary(self.http_status_latencies),
                "http_config": self.latency_summary(self.http_config_latencies),
                "bacnet": self.latency_summary(self.bacnet_latencies),
            },
            "success": not interrupted
            and self.request_failures == 0
            and self.samples_with_alerts == 0,
        }


class JsonlLog:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        try:
            self.file = path.open("x", encoding="utf-8", buffering=1)
        except FileExistsError as error:
            raise SoakError(f"refusing to overwrite existing log: {path}") from error
        self.path = path
        self.rows = 0

    def write(self, value: dict[str, Any]) -> None:
        self.file.write(json.dumps(value, separators=(",", ":"), sort_keys=True) + "\n")
        self.file.flush()
        self.rows += 1
        if self.rows % 10 == 0:
            os.fsync(self.file.fileno())

    def close(self) -> None:
        self.file.flush()
        os.fsync(self.file.fileno())
        self.file.close()


def validate_args(args: argparse.Namespace) -> None:
    try:
        address = ipaddress.ip_address(args.device_address)
    except ValueError as error:
        raise SoakError(f"invalid device IPv4 address: {error}") from error
    if address.version != 4:
        raise SoakError("device address must be IPv4")
    if not 0 <= args.device_instance <= 4_194_302:
        raise SoakError("device instance must be 0-4194302")
    if not 1 <= args.bacnet_port <= 65535:
        raise SoakError("BACnet port must be 1-65535")
    timing_values = (args.duration, args.interval, args.timeout)
    if not all(math.isfinite(value) for value in timing_values):
        raise SoakError("duration, interval, and timeout must be finite")
    if args.duration <= 0 or args.interval <= 0 or args.timeout <= 0:
        raise SoakError("duration, interval, and timeout must be greater than zero")
    if args.interval < args.timeout * 2:
        raise SoakError("interval must be at least twice the per-request timeout")
    if args.summary_every < 1:
        raise SoakError("summary interval must be at least one sample")
    if not 0 <= args.expected_relay_mask <= 255:
        raise SoakError("expected relay mask must be 0-255")
    if args.minimum_heap_bytes < 0:
        raise SoakError("minimum heap threshold cannot be negative")


def parse_integer(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-address", required=True)
    parser.add_argument("--device-instance", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path, help="new JSONL log path")
    parser.add_argument("--duration", type=float, default=86_400.0, help="seconds to monitor")
    parser.add_argument("--interval", type=float, default=60.0, help="seconds between samples")
    parser.add_argument("--timeout", type=float, default=5.0, help="per-request timeout")
    parser.add_argument("--bacnet-port", type=int, default=BACNET_PORT)
    parser.add_argument(
        "--expected-relay-mask",
        type=parse_integer,
        default=0,
        help="expected physical/command mask; accepts decimal or 0xNN",
    )
    parser.add_argument(
        "--minimum-heap-bytes",
        type=int,
        default=200_000,
        help="alert threshold for current and historical free heap",
    )
    parser.add_argument(
        "--summary-every",
        type=int,
        default=15,
        help="print a concise progress line every N samples",
    )
    return parser


def sample_schedule(duration: float, interval: float):
    """Yield elapsed sample times, including both zero and the duration boundary."""
    yield 0.0
    offset = interval
    while offset < duration:
        yield offset
        offset += interval
    yield duration


def take_sample(
    args: argparse.Namespace,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any], float, float]:
    bacnet = probe_bacnet(
        args.device_address, args.device_instance, args.bacnet_port, args.timeout
    )
    status, status_ms = fetch_json(args.device_address, "/api/v1/status", args.timeout)
    config, config_ms = fetch_json(args.device_address, "/api/v1/config", args.timeout)
    return status, config, bacnet, status_ms, config_ms


def progress_line(stats: MonitorStats, status: dict[str, Any] | None) -> str:
    if status is None:
        return (
            f"samples={stats.samples} ok={stats.successful_samples} "
            f"failures={stats.request_failures} alerts={stats.samples_with_alerts}"
        )
    return (
        f"samples={stats.samples} ok={stats.successful_samples} "
        f"failures={stats.request_failures} alerts={stats.samples_with_alerts} "
        f"uptime={status.get('uptime_seconds')}s "
        f"heap={status.get('free_heap_bytes')}/{status.get('minimum_free_heap_bytes')} "
        f"packets={status.get('bacnet_packets_received')}"
    )


def run_monitor(args: argparse.Namespace) -> int:
    validate_args(args)
    log = JsonlLog(args.output)
    started_monotonic = time.monotonic()
    started_at = utc_now()
    stats = MonitorStats(args.duration, started_at)
    baseline: Baseline | None = None
    previous_status: dict[str, Any] | None = None
    interrupted = False
    print(f"Soak log: {args.output}", flush=True)
    try:
        for sequence, scheduled_offset in enumerate(
            sample_schedule(args.duration, args.interval)
        ):
            scheduled = started_monotonic + scheduled_offset
            now = time.monotonic()
            if now < scheduled:
                time.sleep(scheduled - now)
            sampled_at = utc_now()
            sample_started = time.monotonic()
            try:
                status, config, bacnet, status_ms, config_ms = take_sample(args)
                if baseline is None:
                    candidate = Baseline.from_values(status, config)
                    if candidate.device_instance != args.device_instance:
                        raise SoakError(
                            "HTTP Device instance "
                            f"{candidate.device_instance} does not match target"
                        )
                    if candidate.ip_address != args.device_address:
                        raise SoakError(
                            f"HTTP address {candidate.ip_address} does not match target"
                        )
                    if candidate.bacnet_port != args.bacnet_port:
                        raise SoakError(
                            f"HTTP BACnet port {candidate.bacnet_port} does not match target"
                        )
                    baseline = candidate
                    log.write(
                        {
                            "type": "baseline",
                            "timestamp": sampled_at,
                            "target": {
                                "device_address": args.device_address,
                                "device_instance": args.device_instance,
                                "bacnet_port": args.bacnet_port,
                            },
                            "monitor": {
                                "duration_seconds": args.duration,
                                "interval_seconds": args.interval,
                                "timeout_seconds": args.timeout,
                                "expected_relay_mask": args.expected_relay_mask,
                                "minimum_heap_bytes": args.minimum_heap_bytes,
                            },
                            "baseline": baseline.__dict__,
                            "configuration": config,
                        }
                    )
                alerts = evaluate_sample(
                    baseline,
                    previous_status,
                    status,
                    config,
                    bacnet,
                    expected_relay_mask=args.expected_relay_mask,
                    minimum_heap_bytes=args.minimum_heap_bytes,
                )
                stats.record_success(
                    status,
                    status_ms,
                    config_ms,
                    float(bacnet["latency_ms"]),
                    alerts,
                )
                record = {
                    "type": "sample",
                    "sequence": sequence,
                    "timestamp": sampled_at,
                    "elapsed_seconds": round(time.monotonic() - started_monotonic, 3),
                    "sample_duration_ms": round(
                        (time.monotonic() - sample_started) * 1000.0, 3
                    ),
                    "ok": not alerts,
                    "alerts": alerts,
                    "http_status_latency_ms": round(status_ms, 3),
                    "http_config_latency_ms": round(config_ms, 3),
                    "bacnet": bacnet,
                    "config_database_revision": config.get("database_revision"),
                    "config_sha256": config_fingerprint(config),
                    "status": status,
                }
                previous_status = status
                if alerts:
                    print(
                        f"ALERT sample {sequence}: {'; '.join(alerts)}", file=sys.stderr, flush=True
                    )
            except (KeyboardInterrupt, SystemExit):
                raise
            except Exception as error:
                category = type(error).__name__
                stats.record_failure(category)
                record = {
                    "type": "sample",
                    "sequence": sequence,
                    "timestamp": sampled_at,
                    "elapsed_seconds": round(time.monotonic() - started_monotonic, 3),
                    "sample_duration_ms": round(
                        (time.monotonic() - sample_started) * 1000.0, 3
                    ),
                    "ok": False,
                    "alerts": [f"request-failure:{category}"],
                    "error": f"{category}: {error}",
                }
                print(f"ALERT sample {sequence}: {record['error']}", file=sys.stderr, flush=True)
            log.write(record)
            if stats.samples % args.summary_every == 0 or not record["ok"]:
                print(progress_line(stats, previous_status), flush=True)
    except KeyboardInterrupt:
        interrupted = True
        print("Soak monitor interrupted", file=sys.stderr, flush=True)
    finally:
        finished_at = utc_now()
        summary = stats.summary(
            finished_at=finished_at,
            elapsed_seconds=time.monotonic() - started_monotonic,
            interrupted=interrupted,
        )
        log.write(summary)
        log.close()
        print(json.dumps(summary, indent=2, sort_keys=True), flush=True)
    if interrupted:
        return 130
    return 0 if summary["success"] else 1


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return run_monitor(args)
    except SoakError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"error: cannot open or update soak log: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
