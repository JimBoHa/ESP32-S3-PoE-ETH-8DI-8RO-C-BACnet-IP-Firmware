#!/usr/bin/env python3
"""Commission and update the BACnet I/O firmware over Ethernet."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import ProxyHandler, Request, build_opener


AUTH_VERSION = "BACNET-IO-AUTH-V1"
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 24 + 8
APP_DESC_SIZE = 256
MAX_OTA_SIZE = 0x600000
EXPECTED_PROJECT = "esp32_s3_poe_eth_8di_8ro_bacnet"[:31]
MAX_RESPONSE_SIZE = 1024 * 1024
DIRECT_OPENER = build_opener(ProxyHandler({}))


class AdminError(RuntimeError):
    """Expected command-line or device error."""


def normalize_device(value: str) -> str:
    if "://" not in value:
        value = "http://" + value
    parsed = urlsplit(value)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise AdminError("device must be an IPv4 address, hostname, or HTTP URL")
    if parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise AdminError("device URL must not contain credentials, a query, or a fragment")
    if parsed.path not in {"", "/"}:
        raise AdminError("device URL must not contain a path")
    try:
        port = parsed.port
    except ValueError as error:
        raise AdminError(f"invalid device URL port: {error}") from error
    host = parsed.hostname
    if ":" in host:
        host = f"[{host}]"
    return f"{parsed.scheme}://{host}" + (f":{port}" if port else "")


def load_admin_key(path: Path) -> bytes:
    try:
        text = path.read_text(encoding="ascii").strip()
    except (OSError, UnicodeError) as error:
        raise AdminError(f"cannot read admin key file {path}: {error}") from error
    if not re.fullmatch(r"[0-9a-fA-F]{64}", text):
        raise AdminError("admin key file must contain exactly 64 hexadecimal characters")
    if os.name == "posix":
        mode = stat.S_IMODE(path.stat().st_mode)
        if mode & 0o077:
            print(
                f"warning: {path} permissions are {mode:04o}; use chmod 600",
                file=sys.stderr,
            )
    return bytes.fromhex(text)


def content_sha256(body: bytes) -> str:
    return hashlib.sha256(body).hexdigest()


def canonical_request(
    method: str, path: str, nonce: str, body: bytes, body_hash: str | None = None
) -> bytes:
    digest = body_hash or content_sha256(body)
    return (
        f"{AUTH_VERSION}\n{method}\n{path}\n{nonce}\n{len(body)}\n{digest}\n"
    ).encode("ascii")


def request_bytes(
    base_url: str,
    path: str,
    *,
    method: str = "GET",
    body: bytes | None = None,
    headers: dict[str, str] | None = None,
    timeout: float = 20.0,
) -> tuple[int, bytes]:
    request_headers = {"Accept": "application/json", "User-Agent": "bacnet-io-admin/0.1"}
    if headers:
        request_headers.update(headers)
    request = Request(
        base_url + path,
        data=body,
        headers=request_headers,
        method=method,
    )
    try:
        with DIRECT_OPENER.open(request, timeout=timeout) as response:
            payload = response.read(MAX_RESPONSE_SIZE + 1)
            if len(payload) > MAX_RESPONSE_SIZE:
                raise AdminError("device response exceeds 1 MiB")
            return response.status, payload
    except HTTPError as error:
        payload = error.read(MAX_RESPONSE_SIZE)
        message = payload.decode("utf-8", errors="replace").strip()
        try:
            detail = json.loads(message).get("error", message)
        except (json.JSONDecodeError, AttributeError):
            detail = message
        raise AdminError(f"device returned HTTP {error.code}: {detail}") from error
    except URLError as error:
        raise AdminError(f"cannot reach {base_url}: {error.reason}") from error
    except TimeoutError as error:
        raise AdminError(f"request to {base_url} timed out") from error


def request_json(base_url: str, path: str, *, timeout: float) -> Any:
    _status, payload = request_bytes(base_url, path, timeout=timeout)
    try:
        return json.loads(payload)
    except json.JSONDecodeError as error:
        raise AdminError(f"device returned invalid JSON from {path}") from error


def issue_challenge(base_url: str, *, timeout: float) -> str:
    response = request_json(base_url, "/api/v1/auth/challenge", timeout=timeout)
    if not isinstance(response, dict) or response.get("scheme") != "HMAC-SHA256":
        raise AdminError("device returned an unsupported authentication challenge")
    nonce = response.get("nonce")
    if not isinstance(nonce, str) or not re.fullmatch(r"[0-9a-fA-F]{32}", nonce):
        raise AdminError("device returned a malformed authentication nonce")
    return nonce.lower()


def authenticated_request(
    base_url: str,
    key: bytes,
    method: str,
    path: str,
    body: bytes,
    *,
    content_type: str,
    timeout: float,
) -> Any:
    nonce = issue_challenge(base_url, timeout=timeout)
    body_hash = content_sha256(body)
    canonical = canonical_request(method, path, nonce, body, body_hash)
    signature = hmac.new(key, canonical, hashlib.sha256).hexdigest()
    headers = {
        "Content-Type": content_type,
        "X-Auth-Nonce": nonce,
        "X-Content-SHA256": body_hash,
        "X-Authorization": signature,
    }
    _status, payload = request_bytes(
        base_url,
        path,
        method=method,
        body=body if body else b"",
        headers=headers,
        timeout=timeout,
    )
    try:
        return json.loads(payload)
    except json.JSONDecodeError as error:
        raise AdminError(f"device returned invalid JSON from {path}") from error


def decode_descriptor_text(field: bytes) -> str:
    return field.split(b"\0", 1)[0].decode("ascii", errors="strict")


def inspect_ota_image(image: bytes) -> dict[str, str]:
    if not image or image[0] != 0xE9:
        raise AdminError("OTA file is not an ESP application image (missing 0xE9 magic)")
    if len(image) < APP_DESC_OFFSET + APP_DESC_SIZE:
        raise AdminError("OTA file is too small to contain an ESP app descriptor")
    magic = struct.unpack_from("<I", image, APP_DESC_OFFSET)[0]
    if magic != APP_DESC_MAGIC:
        raise AdminError("OTA file has no valid ESP app descriptor")
    try:
        version = decode_descriptor_text(image[APP_DESC_OFFSET + 16 : APP_DESC_OFFSET + 48])
        project = decode_descriptor_text(image[APP_DESC_OFFSET + 48 : APP_DESC_OFFSET + 80])
        idf_version = decode_descriptor_text(image[APP_DESC_OFFSET + 112 : APP_DESC_OFFSET + 144])
    except UnicodeDecodeError as error:
        raise AdminError("OTA app descriptor contains invalid text") from error
    if project != EXPECTED_PROJECT:
        raise AdminError(
            f"OTA project is {project!r}; expected {EXPECTED_PROJECT!r}"
        )
    return {"project": project, "version": version, "idf_version": idf_version}


def print_json(value: Any) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


def require_key(args: argparse.Namespace) -> bytes:
    if not args.key_file:
        raise AdminError("this command requires --key-file")
    return load_admin_key(args.key_file)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="device IP, hostname, or base URL")
    parser.add_argument(
        "--key-file",
        type=Path,
        default=Path(os.environ["BACNET_IO_ADMIN_KEY_FILE"])
        if os.environ.get("BACNET_IO_ADMIN_KEY_FILE")
        else None,
        help="file containing the 32-byte admin key as 64 hex characters",
    )
    parser.add_argument("--timeout", type=float, default=20.0, help="network timeout in seconds")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="read device status")
    commands.add_parser("config-get", help="read persistent configuration")

    config_set = commands.add_parser("config-set", help="save a partial or full JSON configuration")
    config_set.add_argument("--file", type=Path, required=True, help="JSON configuration file")

    ota = commands.add_parser("ota", help="upload an app-only firmware image over Ethernet")
    ota.add_argument("--file", type=Path, required=True, help="firmware-ota.bin")
    ota.add_argument("--yes", action="store_true", help="confirm firmware activation and reboot")

    reboot = commands.add_parser("reboot", help="reboot the device")
    reboot.add_argument("--yes", action="store_true", help="confirm reboot")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.timeout <= 0:
        raise AdminError("timeout must be greater than zero")
    base_url = normalize_device(args.device)

    if args.command == "status":
        print_json(request_json(base_url, "/api/v1/status", timeout=args.timeout))
        return 0
    if args.command == "config-get":
        print_json(request_json(base_url, "/api/v1/config", timeout=args.timeout))
        return 0

    key = require_key(args)
    if args.command == "config-set":
        try:
            config = json.loads(args.file.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise AdminError(f"cannot read JSON configuration {args.file}: {error}") from error
        if not isinstance(config, dict):
            raise AdminError("configuration file must contain a JSON object")
        body = json.dumps(config, separators=(",", ":"), sort_keys=True).encode("utf-8")
        print_json(
            authenticated_request(
                base_url,
                key,
                "PUT",
                "/api/v1/config",
                body,
                content_type="application/json",
                timeout=args.timeout,
            )
        )
        return 0
    if args.command == "ota":
        if not args.yes:
            raise AdminError("OTA changes the boot partition; repeat with --yes")
        try:
            image = args.file.read_bytes()
        except OSError as error:
            raise AdminError(f"cannot read OTA image {args.file}: {error}") from error
        if len(image) > MAX_OTA_SIZE:
            raise AdminError(f"OTA image exceeds the {MAX_OTA_SIZE}-byte app partition")
        description = inspect_ota_image(image)
        print(
            f"Uploading {description['project']} {description['version']} "
            f"({len(image)} bytes, SHA-256 {content_sha256(image)})",
            file=sys.stderr,
        )
        print_json(
            authenticated_request(
                base_url,
                key,
                "POST",
                "/api/v1/ota",
                image,
                content_type="application/octet-stream",
                timeout=max(args.timeout, 120.0),
            )
        )
        return 0
    if args.command == "reboot":
        if not args.yes:
            raise AdminError("reboot interrupts I/O service; repeat with --yes")
        print_json(
            authenticated_request(
                base_url,
                key,
                "POST",
                "/api/v1/reboot",
                b"",
                content_type="application/octet-stream",
                timeout=args.timeout,
            )
        )
        return 0
    raise AdminError(f"unsupported command {args.command}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AdminError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
