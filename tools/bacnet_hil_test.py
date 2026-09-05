#!/usr/bin/env python3
"""Run repeatable BACnet/IP hardware-in-the-loop acceptance tests."""

from __future__ import annotations

import argparse
import asyncio
from contextlib import AsyncExitStack
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
import importlib.metadata
import ipaddress
import json
import os
from pathlib import Path
import re
import sys
import time
from typing import Any, Awaitable
from urllib.request import ProxyHandler, build_opener


REQUIRED_BACPYPES3_VERSION = "0.0.106"
BACNET_PORT = 47808
MAX_DEVICE_INSTANCE = 4_194_302
RELAY_COUNT = 8
INPUT_COUNT = 8
RELAY_CONFIRMATION = "loads-disconnected"
MAX_HTTP_RESPONSE_SIZE = 1024 * 1024


class HilError(RuntimeError):
    """Expected HIL runner failure."""


@dataclass
class CheckResult:
    name: str
    outcome: str
    detail: str
    elapsed_seconds: float = 0.0


@dataclass
class TestReport:
    started_at: str
    target: dict[str, Any]
    options: dict[str, Any]
    bacpypes3_version: str = "unavailable"
    finished_at: str | None = None
    checks: list[CheckResult] = field(default_factory=list)

    def add(
        self, name: str, outcome: str, detail: str, elapsed_seconds: float = 0.0
    ) -> None:
        self.checks.append(CheckResult(name, outcome, detail, elapsed_seconds))
        marker = {"pass": "PASS", "fail": "FAIL", "skip": "SKIP"}[outcome]
        print(f"[{marker}] {name}: {detail}", flush=True)

    def require(
        self, name: str, condition: bool, detail: str, failure_detail: str
    ) -> None:
        self.add(name, "pass" if condition else "fail", detail if condition else failure_detail)
        if not condition:
            raise HilError(f"{name}: {failure_detail}")

    def finish(self) -> None:
        self.finished_at = utc_now()

    def serializable(self) -> dict[str, Any]:
        data = asdict(self)
        outcomes = [check.outcome for check in self.checks]
        data["summary"] = {
            "passed": outcomes.count("pass"),
            "failed": outcomes.count("fail"),
            "skipped": outcomes.count("skip"),
            "success": "fail" not in outcomes,
        }
        return data


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def expected_object_identifiers(device_instance: int) -> set[str]:
    objects = {f"analog-input,{instance}" for instance in range(1001, 1005)}
    objects.update(f"binary-input,{instance}" for instance in range(1, 9))
    objects.update(f"binary-input,{instance}" for instance in range(1001, 1005))
    objects.update(f"binary-output,{instance}" for instance in range(1, 9))
    objects.update(
        {
            "binary-value,1",
            f"device,{device_instance}",
            "characterstring-value,1",
            "network-port,1",
        }
    )
    return objects


def priority_array_is_clear(value: Any) -> bool:
    try:
        return len(value) == 16 and all(
            getattr(item, "_choice", None) == "null" for item in value
        )
    except TypeError:
        return False


def error_signature(error: BaseException) -> tuple[str, str]:
    return (
        str(getattr(error, "errorClass", "")).replace("_", "-").lower(),
        str(getattr(error, "errorCode", "")).replace("_", "-").lower(),
    )


def json_safe(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, dict):
        return {str(key): json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [json_safe(item) for item in value]
    return str(value)


def fetch_http_status(device_address: str, timeout: float) -> dict[str, Any]:
    opener = build_opener(ProxyHandler({}))
    with opener.open(
        f"http://{device_address}/api/v1/status", timeout=timeout
    ) as response:
        payload = response.read(MAX_HTTP_RESPONSE_SIZE + 1)
    if len(payload) > MAX_HTTP_RESPONSE_SIZE:
        raise HilError("management status response exceeds 1 MiB")
    decoded = json.loads(payload)
    if not isinstance(decoded, dict):
        raise HilError("management status response is not a JSON object")
    return decoded


def validate_args(args: argparse.Namespace) -> None:
    if not (0 <= args.device_instance <= MAX_DEVICE_INSTANCE):
        raise HilError(f"device instance must be 0-{MAX_DEVICE_INSTANCE}")
    if not (0 <= args.client_instance <= MAX_DEVICE_INSTANCE):
        raise HilError(f"client instance must be 0-{MAX_DEVICE_INSTANCE}")
    if args.client_instance == args.device_instance:
        raise HilError("client and target Device instances must differ")
    try:
        local = ipaddress.ip_interface(args.local_address)
    except ValueError as error:
        raise HilError(f"invalid --local-address: {error}") from error
    if local.version != 4 or local.network.prefixlen == 32:
        raise HilError("--local-address must be an IPv4 interface with a subnet prefix")
    try:
        target = ipaddress.ip_address(args.device_address)
    except ValueError as error:
        raise HilError(f"invalid --device-address: {error}") from error
    if target.version != 4:
        raise HilError("--device-address must be IPv4")
    if target not in local.network:
        raise HilError("target must be on the --local-address subnet for this local-BACnet test")
    if args.timeout <= 0:
        raise HilError("timeout must be greater than zero")
    if not 3.0 <= args.relay_seconds <= 30.0:
        raise HilError("relay duration must be between 3 and 30 seconds")
    if args.exercise_relays and args.relay_safety_confirmation != RELAY_CONFIRMATION:
        raise HilError(
            "relay testing requires --relay-safety-confirmation loads-disconnected"
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--local-address",
        required=True,
        help="host IPv4 interface in address/prefix form, for example 192.168.75.191/24",
    )
    parser.add_argument("--device-address", required=True, help="target controller IPv4 address")
    parser.add_argument("--device-instance", required=True, type=int)
    parser.add_argument(
        "--client-instance",
        type=int,
        default=4_194_000 + (os.getpid() % 300),
        help="temporary local BACnet Device instance",
    )
    parser.add_argument("--timeout", type=float, default=4.0, help="request timeout in seconds")
    parser.add_argument(
        "--report", type=Path, help="optional path for a machine-readable JSON report"
    )
    parser.add_argument(
        "--exercise-relays",
        action="store_true",
        help="actually command each physical relay through BACnet",
    )
    parser.add_argument(
        "--relay-safety-confirmation",
        help=f"required value when exercising relays: {RELAY_CONFIRMATION}",
    )
    parser.add_argument(
        "--relay-seconds",
        type=float,
        default=3.0,
        help="seconds each relay remains active (3-30)",
    )
    parser.add_argument(
        "--skip-capacity-test",
        action="store_true",
        help="skip the 16-subscription capacity/bounds check",
    )
    return parser


def load_bacpypes3() -> dict[str, Any]:
    try:
        from bacpypes3.app import Application
        from bacpypes3.pdu import Address
        from bacpypes3.primitivedata import CharacterString, Null, ObjectIdentifier
    except ImportError as error:
        raise HilError(
            "bacpypes3 is required; install with "
            "python -m pip install -r requirements-hil.txt"
        ) from error
    return {
        "Application": Application,
        "Address": Address,
        "CharacterString": CharacterString,
        "Null": Null,
        "ObjectIdentifier": ObjectIdentifier,
    }


class HilRunner:
    def __init__(self, args: argparse.Namespace, report: TestReport, runtime: dict[str, Any]):
        self.args = args
        self.report = report
        self.runtime = runtime
        self.destination = runtime["Address"](args.device_address)
        self.app: Any = None
        self.device_name = ""
        self.object_list: list[Any] = []

    async def operation(self, name: str, awaitable: Awaitable[Any]) -> Any:
        started = time.monotonic()
        try:
            result = await asyncio.wait_for(awaitable, timeout=self.args.timeout)
        except (KeyboardInterrupt, SystemExit):
            raise
        except BaseException as error:
            elapsed = time.monotonic() - started
            self.report.add(name, "fail", f"{type(error).__name__}: {error}", elapsed)
            raise HilError(f"{name} failed") from error
        elapsed = time.monotonic() - started
        self.report.add(name, "pass", "response received", elapsed)
        return result

    async def read(self, object_identifier: str, prop: str, index: int | None = None) -> Any:
        return await asyncio.wait_for(
            self.app.read_property(self.destination, object_identifier, prop, index),
            timeout=self.args.timeout,
        )

    async def expect_error(
        self,
        name: str,
        awaitable: Awaitable[Any],
        expected_class: str,
        expected_code: str,
    ) -> None:
        started = time.monotonic()
        try:
            result = await asyncio.wait_for(awaitable, timeout=self.args.timeout)
        except (KeyboardInterrupt, SystemExit):
            raise
        except BaseException as error:
            signature = error_signature(error)
            expected = (expected_class, expected_code)
            if signature == expected:
                self.report.add(
                    name,
                    "pass",
                    f"rejected with {signature[0]}:{signature[1]}",
                    time.monotonic() - started,
                )
                return
            self.report.add(
                name,
                "fail",
                f"wrong error {signature[0]}:{signature[1]} ({error})",
                time.monotonic() - started,
            )
            raise HilError(f"{name} returned the wrong BACnet error") from error
        self.report.add(
            name,
            "fail",
            f"request unexpectedly succeeded: {json_safe(result)}",
            time.monotonic() - started,
        )
        raise HilError(f"{name} unexpectedly succeeded")

    async def run(self) -> None:
        app_args = argparse.Namespace(
            vendoridentifier=999,
            instance=self.args.client_instance,
            name="BACnet HIL Acceptance Client",
            address=self.args.local_address,
            network=0,
            foreign=None,
            ttl=30,
            bbmd=None,
        )
        self.app = self.runtime["Application"].from_args(app_args)
        try:
            await self.test_discovery()
            await self.test_object_model()
            await self.test_rpm()
            await self.test_who_has()
            await self.test_cov()
            if self.args.skip_capacity_test:
                self.report.add("COV capacity bound", "skip", "disabled by command line")
            else:
                await self.test_cov_capacity()
            await self.test_negative_access()
            if self.args.exercise_relays:
                await self.test_relays()
            else:
                self.report.add(
                    "Physical relay sequence",
                    "skip",
                    "requires --exercise-relays and explicit disconnected-load confirmation",
                )
        finally:
            if self.app is not None:
                self.app.close()

    async def test_discovery(self) -> None:
        directed = await self.operation(
            "Directed Who-Is/I-Am",
            self.app.who_is(
                self.args.device_instance,
                self.args.device_instance,
                address=self.destination,
                timeout=self.args.timeout,
            ),
        )
        broadcast = await self.operation(
            "Broadcast Who-Is/I-Am",
            self.app.who_is(
                self.args.device_instance,
                self.args.device_instance,
                timeout=self.args.timeout,
            ),
        )
        for name, responses in (("directed", directed), ("broadcast", broadcast)):
            matches = [
                response
                for response in responses
                if int(response.iAmDeviceIdentifier[1]) == self.args.device_instance
            ]
            self.report.require(
                f"{name.title()} discovery identity",
                len(matches) == 1,
                "one matching I-Am received",
                f"expected one matching I-Am, received {len(matches)}",
            )
            response = matches[0]
            self.report.require(
                f"{name.title()} I-Am capabilities",
                int(response.maxAPDULengthAccepted) == 1476
                and str(response.segmentationSupported) == "no-segmentation"
                and 0 <= int(response.vendorID) <= 65535,
                f"APDU=1476, segmentation={response.segmentationSupported}, vendor={response.vendorID}",
                "I-Am capability fields are unexpected",
            )

    async def test_object_model(self) -> None:
        object_count = await self.read(f"device,{self.args.device_instance}", "object-list", 0)
        self.object_list = list(
            await self.read(f"device,{self.args.device_instance}", "object-list")
        )
        actual_objects = {str(value) for value in self.object_list}
        expected_objects = expected_object_identifiers(self.args.device_instance)
        self.report.require(
            "Complete Object_List",
            int(object_count) == len(self.object_list) == len(expected_objects)
            and actual_objects == expected_objects,
            f"all {len(expected_objects)} expected objects present",
            f"missing={sorted(expected_objects - actual_objects)}, "
            f"unexpected={sorted(actual_objects - expected_objects)}",
        )

        device = f"device,{self.args.device_instance}"
        metadata_properties = (
            "object-identifier",
            "object-name",
            "description",
            "system-status",
            "vendor-name",
            "vendor-identifier",
            "model-name",
            "firmware-revision",
            "application-software-version",
            "protocol-version",
            "protocol-revision",
            "protocol-services-supported",
            "protocol-object-types-supported",
            "max-apdu-length-accepted",
            "segmentation-supported",
            "apdu-timeout",
            "number-of-apdu-retries",
            "database-revision",
            "location",
            "serial-number",
        )
        metadata = {
            prop: await self.read(device, prop) for prop in metadata_properties
        }
        self.device_name = str(metadata["object-name"])
        strings_ok = all(
            str(metadata[prop]).strip()
            for prop in (
                "object-name",
                "description",
                "vendor-name",
                "model-name",
                "firmware-revision",
                "application-software-version",
                "location",
                "serial-number",
            )
        )
        serial = str(metadata["serial-number"])
        self.report.require(
            "Device identity metadata",
            strings_ok
            and str(metadata["object-identifier"]) == device
            and bool(re.fullmatch(r"[0-9A-F]{12}", serial)),
            f"name={self.device_name!r}, firmware={metadata['firmware-revision']}, serial={serial}",
            "one or more Device identity fields are missing or malformed",
        )
        self.report.require(
            "Device protocol metadata",
            int(metadata["max-apdu-length-accepted"]) == 1476
            and str(metadata["segmentation-supported"]) == "no-segmentation"
            and int(metadata["protocol-revision"]) >= 22
            and int(metadata["apdu-timeout"]) > 0
            and int(metadata["number-of-apdu-retries"]) >= 0,
            f"protocol revision {metadata['protocol-revision']}; no segmentation; APDU 1476",
            "Device protocol metadata does not match the implementation profile",
        )
        expected_services = {
            "subscribe-cov",
            "read-property",
            "read-property-multiple",
            "write-property",
            "who-has",
            "who-is",
        }
        actual_services = set(str(metadata["protocol-services-supported"]).split(";"))
        expected_types = {
            "analog-input",
            "binary-input",
            "binary-output",
            "binary-value",
            "device",
            "characterstring-value",
            "network-port",
        }
        actual_types = set(str(metadata["protocol-object-types-supported"]).split(";"))
        self.report.require(
            "Advertised BACnet capabilities",
            expected_services.issubset(actual_services)
            and expected_types.issubset(actual_types),
            "all implemented services and object types are advertised",
            f"missing services={sorted(expected_services - actual_services)}, "
            f"missing object types={sorted(expected_types - actual_types)}",
        )

        names: list[str] = [self.device_name]
        for instance in range(1, INPUT_COUNT + 1):
            obj = f"binary-input,{instance}"
            name = str(await self.read(obj, "object-name"))
            description = str(await self.read(obj, "description"))
            polarity = str(await self.read(obj, "polarity"))
            present = str(await self.read(obj, "present-value"))
            names.append(name)
            self.report.require(
                f"DI{instance} metadata",
                bool(name.strip())
                and f"DI{instance}" in description
                and polarity in {"normal", "reverse"}
                and present in {"inactive", "active"},
                f"{name!r}; {polarity}; {present}",
                "name, description, polarity, or Present_Value is invalid",
            )
        for instance in range(1, RELAY_COUNT + 1):
            obj = f"binary-output,{instance}"
            name = str(await self.read(obj, "object-name"))
            description = str(await self.read(obj, "description"))
            present = str(await self.read(obj, "present-value"))
            priority_array = await self.read(obj, "priority-array")
            names.append(name)
            self.report.require(
                f"RO{instance} safe baseline",
                bool(name.strip())
                and f"RO{instance}" in description
                and present == "inactive"
                and priority_array_is_clear(priority_array),
                f"{name!r}; inactive; all priorities clear",
                "relay is active, commanded, or has malformed metadata",
            )
        status_objects = [
            *(f"binary-input,{instance}" for instance in range(1001, 1005)),
            *(f"analog-input,{instance}" for instance in range(1001, 1005)),
            "binary-value,1",
            "characterstring-value,1",
            "network-port,1",
        ]
        for obj in status_objects:
            name = str(await self.read(obj, "object-name"))
            names.append(name)
            self.report.require(
                f"{obj} discoverable",
                bool(name.strip()),
                f"Object_Name={name!r}",
                "Object_Name is empty",
            )
        folded = [name.casefold() for name in names]
        self.report.require(
            "BACnet Object_Name uniqueness",
            len(folded) == len(set(folded)) == len(expected_objects),
            f"all {len(names)} object names are unique",
            "empty, duplicate, or missing Object_Name values found",
        )

    async def test_rpm(self) -> None:
        property_count = 0
        for obj in self.object_list:
            results = await asyncio.wait_for(
                self.app.read_property_multiple(
                    self.destination,
                    [obj, ["all"]],
                ),
                timeout=self.args.timeout,
            )
            if isinstance(results, BaseException) or not isinstance(results, list):
                raise HilError(f"RPM ALL failed for {obj}: {results}")
            errors = [
                value
                for _result_obj, _prop, _index, value in results
                if hasattr(value, "errorClass") or value is None
            ]
            if errors:
                raise HilError(f"RPM ALL returned property errors for {obj}: {errors}")
            property_count += len(results)
        self.report.add(
            "ReadPropertyMultiple ALL scan",
            "pass",
            f"{property_count} property values across {len(self.object_list)} objects",
        )

    async def test_who_has(self) -> None:
        character_string = self.runtime["CharacterString"]
        object_identifier = self.runtime["ObjectIdentifier"]
        cases = (
            (
                "Who-Has by object identifier",
                {"object_identifier": object_identifier("binary-output,1")},
                "binary-output,1",
            ),
            (
                "Who-Has by configured name",
                {
                    "object_name": character_string(
                        str(await self.read("binary-output,1", "object-name"))
                    )
                },
                "binary-output,1",
            ),
        )
        for name, query, expected_object in cases:
            responses = await asyncio.wait_for(
                self.app.who_has(
                    self.args.device_instance,
                    self.args.device_instance,
                    address=self.destination,
                    timeout=self.args.timeout,
                    **query,
                ),
                timeout=self.args.timeout + 1.0,
            )
            matched = [
                response
                for response in responses
                if str(response.deviceIdentifier)
                == f"device,{self.args.device_instance}"
                and str(response.objectIdentifier) == expected_object
            ]
            self.report.require(
                name,
                len(matched) == 1,
                f"I-Have identified {expected_object}",
                f"expected one matching I-Have, received {len(matched)}",
            )

    async def next_cov_present_value(
        self, subscription: Any, expected: str, timeout: float | None = None
    ) -> None:
        deadline = time.monotonic() + (timeout or self.args.timeout)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise asyncio.TimeoutError(f"no COV Present_Value={expected}")
            prop, value = await asyncio.wait_for(subscription.get_value(), remaining)
            if str(prop) == "present-value" and str(value) == expected:
                return

    async def test_cov(self) -> None:
        object_identifier = self.runtime["ObjectIdentifier"]
        for confirmed in (True, False):
            mode = "Confirmed" if confirmed else "Unconfirmed"
            async with self.app.change_of_value(
                self.destination,
                object_identifier("binary-input,1"),
                issue_confirmed_notifications=confirmed,
                lifetime=30,
            ) as subscription:
                await self.next_cov_present_value(
                    subscription, str(await self.read("binary-input,1", "present-value"))
                )
            self.report.add(
                f"{mode} COV subscription",
                "pass",
                "subscription, initial notification, and cancellation succeeded",
            )

    async def test_cov_capacity(self) -> None:
        active_before = await self.read(
            f"device,{self.args.device_instance}", "active-cov-subscriptions"
        )
        if len(active_before) != 0:
            self.report.add(
                "COV capacity bound",
                "skip",
                f"device already has {len(active_before)} external subscription(s)",
            )
            return
        object_identifier = self.runtime["ObjectIdentifier"]
        monitored = [
            *(f"binary-input,{instance}" for instance in range(1, 9)),
            *(f"binary-input,{instance}" for instance in range(1001, 1005)),
            *(f"analog-input,{instance}" for instance in range(1001, 1005)),
        ]
        async with AsyncExitStack() as stack:
            for offset, obj in enumerate(monitored):
                await stack.enter_async_context(
                    self.app.change_of_value(
                        self.destination,
                        object_identifier(obj),
                        subscriber_process_identifier=10_000 + offset,
                        issue_confirmed_notifications=bool(offset % 2),
                        lifetime=30,
                    )
                )
            overflow = self.app.change_of_value(
                self.destination,
                object_identifier("binary-output,1"),
                subscriber_process_identifier=10_099,
                issue_confirmed_notifications=True,
                lifetime=30,
            )
            try:
                await asyncio.wait_for(overflow.__aenter__(), timeout=self.args.timeout)
            except (KeyboardInterrupt, SystemExit):
                raise
            except BaseException as error:
                signature = error_signature(error)
                self.report.require(
                    "COV subscription 17 capacity rejection",
                    signature
                    == ("resources", "no-space-to-add-list-element"),
                    "rejected with resources:no-space-to-add-list-element",
                    f"wrong BACnet error {signature[0]}:{signature[1]}",
                )
            else:
                await overflow.__aexit__(None, None, None)
                self.report.require(
                    "COV subscription 17 capacity rejection",
                    False,
                    "",
                    "17th subscription unexpectedly succeeded",
                )
        await asyncio.sleep(0.1)
        active_after = await self.read(
            f"device,{self.args.device_instance}", "active-cov-subscriptions"
        )
        self.report.require(
            "COV capacity cleanup",
            len(active_after) == 0,
            "all 16 temporary subscriptions were cancelled",
            f"{len(active_after)} temporary subscription(s) remain",
        )

    async def test_negative_access(self) -> None:
        character_string = self.runtime["CharacterString"]
        await self.expect_error(
            "Device Object_Name write protection",
            self.app.write_property(
                self.destination,
                f"device,{self.args.device_instance}",
                "object-name",
                character_string(self.device_name),
            ),
            "property",
            "write-access-denied",
        )
        input_value = str(await self.read("binary-input,1", "present-value"))
        await self.expect_error(
            "Binary Input write protection",
            self.app.write_property(
                self.destination, "binary-input,1", "present-value", input_value
            ),
            "property",
            "write-access-denied",
        )
        await self.expect_error(
            "Unknown object rejection",
            self.app.read_property(
                self.destination, "binary-input,4194302", "present-value"
            ),
            "object",
            "unknown-object",
        )
        await self.expect_error(
            "Invalid Object_List index rejection",
            self.app.read_property(
                self.destination,
                f"device,{self.args.device_instance}",
                "object-list",
                9999,
            ),
            "property",
            "invalid-array-index",
        )

    async def all_relays_inactive(self) -> bool:
        values = [
            str(await self.read(f"binary-output,{instance}", "present-value"))
            for instance in range(1, RELAY_COUNT + 1)
        ]
        return values == ["inactive"] * RELAY_COUNT

    async def test_relays(self) -> None:
        null = self.runtime["Null"]
        object_identifier = self.runtime["ObjectIdentifier"]
        touched: set[tuple[int, int]] = set()
        try:
            self.report.require(
                "Relay test safe starting state",
                await self.all_relays_inactive(),
                "all relay Present_Value values are inactive",
                "one or more relays are already active",
            )
            for instance in range(1, RELAY_COUNT + 1):
                obj = f"binary-output,{instance}"
                priority = await self.read(obj, "priority-array")
                self.report.require(
                    f"RO{instance} priority precondition",
                    priority_array_is_clear(priority),
                    "all priority slots clear",
                    "priority array is not clear; refusing to overwrite an external command",
                )
                async with self.app.change_of_value(
                    self.destination,
                    object_identifier(obj),
                    issue_confirmed_notifications=bool(instance % 2),
                    lifetime=30,
                ) as subscription:
                    await self.next_cov_present_value(subscription, "inactive")
                    await asyncio.wait_for(
                        self.app.write_property(
                            self.destination,
                            obj,
                            "present-value",
                            "active",
                            priority=8,
                        ),
                        timeout=self.args.timeout,
                    )
                    touched.add((instance, 8))
                    await self.next_cov_present_value(subscription, "active")
                    active_values = [
                        str(
                            await self.read(
                                f"binary-output,{channel}", "present-value"
                            )
                        )
                        for channel in range(1, RELAY_COUNT + 1)
                    ]
                    expected = ["inactive"] * RELAY_COUNT
                    expected[instance - 1] = "active"
                    self.report.require(
                        f"RO{instance} one-at-a-time command",
                        active_values == expected,
                        f"only RO{instance} active at priority 8",
                        f"unexpected relay states: {active_values}",
                    )
                    status = await asyncio.to_thread(
                        fetch_http_status,
                        self.args.device_address,
                        self.args.timeout,
                    )
                    expected_mask = 1 << (instance - 1)
                    self.report.require(
                        f"RO{instance} hardware command cross-check",
                        status.get("relay_outputs_mask") == expected_mask
                        and status.get("relay_commands_mask") == expected_mask
                        and status.get("relay_controller_healthy") is True
                        and status.get("relay_active_priorities", [])[instance - 1] == 8,
                        f"TCA9554 mask=0x{expected_mask:02x}, priority=8, controller healthy",
                        f"unexpected management status: {json_safe(status)}",
                    )
                    await asyncio.sleep(self.args.relay_seconds)
                    await asyncio.wait_for(
                        self.app.write_property(
                            self.destination,
                            obj,
                            "present-value",
                            null(()),
                            priority=8,
                        ),
                        timeout=self.args.timeout,
                    )
                    touched.discard((instance, 8))
                    await self.next_cov_present_value(subscription, "inactive")
                self.report.require(
                    f"RO{instance} relinquish",
                    await self.all_relays_inactive()
                    and priority_array_is_clear(await self.read(obj, "priority-array")),
                    "inactive with priority array clear",
                    "relay did not return to its safe relinquish default",
                )

            obj = "binary-output,1"
            await self.app.write_property(
                self.destination, obj, "present-value", "active", priority=16
            )
            touched.add((1, 16))
            await self.app.write_property(
                self.destination, obj, "present-value", "inactive", priority=8
            )
            touched.add((1, 8))
            value_at_8 = str(await self.read(obj, "present-value"))
            await self.app.write_property(
                self.destination, obj, "present-value", null(()), priority=8
            )
            touched.discard((1, 8))
            value_at_16 = str(await self.read(obj, "present-value"))
            await self.app.write_property(
                self.destination, obj, "present-value", null(()), priority=16
            )
            touched.discard((1, 16))
            final_value = str(await self.read(obj, "present-value"))
            self.report.require(
                "BACnet relay priority arbitration",
                (value_at_8, value_at_16, final_value)
                == ("inactive", "active", "inactive"),
                "priority 8 overrode 16; relinquish exposed 16; final default was inactive",
                f"unexpected sequence: {value_at_8}, {value_at_16}, {final_value}",
            )
            await self.expect_error(
                "Reserved relay priority 6 rejection",
                self.app.write_property(
                    self.destination,
                    obj,
                    "present-value",
                    "active",
                    priority=6,
                ),
                "property",
                "write-access-denied",
            )
            priorities_clear = True
            for instance in range(1, RELAY_COUNT + 1):
                priority_array = await self.read(
                    f"binary-output,{instance}", "priority-array"
                )
                priorities_clear = (
                    priority_array_is_clear(priority_array) and priorities_clear
                )
            final_status = await asyncio.to_thread(
                fetch_http_status, self.args.device_address, self.args.timeout
            )
            self.report.require(
                "Final relay safe state",
                await self.all_relays_inactive()
                and priorities_clear
                and final_status.get("relay_outputs_mask") == 0
                and final_status.get("relay_commands_mask") == 0
                and final_status.get("relay_active_priorities") == [0] * RELAY_COUNT,
                "all BACnet priorities and physical command masks are clear",
                f"unexpected final management status: {json_safe(final_status)}",
            )
        finally:
            for instance, priority in sorted(touched):
                try:
                    await asyncio.wait_for(
                        self.app.write_property(
                            self.destination,
                            f"binary-output,{instance}",
                            "present-value",
                            null(()),
                            priority=priority,
                        ),
                        timeout=self.args.timeout,
                    )
                except BaseException as error:
                    self.report.add(
                        f"Emergency cleanup RO{instance} priority {priority}",
                        "fail",
                        f"{type(error).__name__}: {error}",
                    )


def write_report(path: Path, report: TestReport) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(report.serializable(), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


async def async_main(args: argparse.Namespace, report: TestReport) -> None:
    runtime = load_bacpypes3()
    report.bacpypes3_version = importlib.metadata.version("bacpypes3")
    if report.bacpypes3_version != REQUIRED_BACPYPES3_VERSION:
        report.add(
            "BACpypes3 version",
            "fail",
            f"found {report.bacpypes3_version}; expected {REQUIRED_BACPYPES3_VERSION}",
        )
        raise HilError("unsupported BACpypes3 version")
    report.add(
        "BACpypes3 version", "pass", f"using pinned version {report.bacpypes3_version}"
    )
    await HilRunner(args, report, runtime).run()


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    report = TestReport(
        started_at=utc_now(),
        target={
            "device_address": args.device_address,
            "device_instance": args.device_instance,
        },
        options={
            "local_address": args.local_address,
            "client_instance": args.client_instance,
            "timeout": args.timeout,
            "exercise_relays": args.exercise_relays,
            "relay_seconds": args.relay_seconds,
            "capacity_test": not args.skip_capacity_test,
        },
    )
    exit_code = 0
    try:
        validate_args(args)
        asyncio.run(async_main(args, report))
    except (KeyboardInterrupt, SystemExit):
        raise
    except HilError as error:
        print(f"error: {error}", file=sys.stderr)
        if not any(check.outcome == "fail" for check in report.checks):
            report.add("HIL runner", "fail", str(error))
        exit_code = 1
    except BaseException as error:
        print(f"error: unexpected HIL runner failure: {error}", file=sys.stderr)
        report.add(
            "HIL runner internal error", "fail", f"{type(error).__name__}: {error}"
        )
        exit_code = 1
    finally:
        report.finish()
        if args.report:
            try:
                write_report(args.report, report)
                print(f"Report: {args.report}", flush=True)
            except OSError as error:
                print(f"error: cannot write report {args.report}: {error}", file=sys.stderr)
                exit_code = 1
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
