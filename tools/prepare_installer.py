#!/usr/bin/env python3
"""Verify release packages and stage immutable firmware for the browser installer."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

try:
    from .device_admin import inspect_ota_image
except ImportError:
    from device_admin import inspect_ota_image

PROJECT = "esp32_s3_poe_eth_8di_8ro_bacnet"
TARGET = "ESP32-S3-PoE-ETH-8DI-8RO-C"
VERSION = re.compile(r"\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?\Z")


def verify_package(directory: Path) -> dict:
    directory = directory.resolve()
    checksums = directory / "SHA256SUMS"
    verified = set()
    for line in checksums.read_text(encoding="ascii").splitlines():
        digest, separator, relative = line.partition("  ")
        if not separator or not re.fullmatch(r"[a-f0-9]{64}", digest):
            raise ValueError("Malformed release checksum list")
        path = (directory / relative).resolve()
        if not path.is_relative_to(directory) or path == directory or relative in verified:
            raise ValueError("Unsafe or duplicate release checksum path")
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Release checksum failed: {relative}")
        verified.add(relative)
    required = {"manifest.json", "initial-flash.bin", "firmware-ota.bin"}
    if not required.issubset(verified):
        raise ValueError("Release checksum list is incomplete")
    manifest = json.loads((directory / "manifest.json").read_text())
    version = manifest.get("version", "")
    if not VERSION.fullmatch(version) or manifest.get("project") != PROJECT or manifest.get("target") != TARGET:
        raise ValueError("Release is for a different firmware project or board")
    if tuple(map(int, version.split("-", 1)[0].split("."))) < (0, 14, 0):
        raise ValueError("Browser setup requires firmware 0.14.0 or newer")
    app = inspect_ota_image((directory / "firmware-ota.bin").read_bytes())
    if app["version"] != version or app["project"] != PROJECT:
        raise ValueError("Release metadata and application descriptor disagree")
    initial = (directory / "initial-flash.bin").read_bytes()
    ota = (directory / "firmware-ota.bin").read_bytes()
    if len(initial) != 0x20000 + len(ota) or initial[0x20000:] != ota:
        raise ValueError("Initial image does not contain the expected application at 0x20000")
    if initial[0] != 0xE9 or initial[12:14] != b"\x09\x00" or initial[3] >> 4 != 4:
        raise ValueError("Initial image is not ESP32-S3 / 16 MB")
    return manifest


def stage(release_directories: list[Path], output: Path, recommended: str) -> dict:
    # Validate every input before changing the existing published catalog.
    packages = [(path.resolve(), verify_package(path)) for path in release_directories]
    versions = [manifest["version"] for _, manifest in packages]
    if len(set(versions)) != len(versions) or recommended not in versions:
        raise ValueError("Recommended release must exist; versions must be unique")
    output.mkdir(parents=True, exist_ok=True)
    releases = []
    for source, manifest in packages:
        version = manifest["version"]
        destination = output / version
        destination.mkdir(exist_ok=True)
        release = {
            "version": version, "project": PROJECT, "chipFamily": "ESP32-S3",
            "flashBytes": 0x1000000, "usbSetupProtocol": 1,
        }
        for kind, filename in (("initial", "initial-flash.bin"), ("ota", "firmware-ota.bin")):
            data = (source / filename).read_bytes()
            target = destination / filename
            if target.exists() and target.read_bytes() != data:
                raise ValueError(f"Refusing to replace immutable firmware {version}/{filename}")
            shutil.copyfile(source / filename, target)
            release[kind] = {"path": f"{version}/{filename}", "bytes": len(data),
                             "sha256": hashlib.sha256(data).hexdigest()}
        releases.append(release)
    releases.sort(key=lambda r: tuple(map(int, r["version"].split("-", 1)[0].split("."))), reverse=True)
    catalog = {"schema": 1, "recommended": recommended, "releases": releases}
    temporary = output / "catalog.json.tmp"
    temporary.write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
    temporary.replace(output / "catalog.json")
    return catalog


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-dir", action="append", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=Path("installer/public/firmware"))
    parser.add_argument("--recommended", required=True)
    args = parser.parse_args()
    catalog = stage(args.release_dir, args.output, args.recommended)
    print(f"Staged {len(catalog['releases'])} verified release(s); recommended {args.recommended}")


if __name__ == "__main__":
    main()
