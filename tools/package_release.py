#!/usr/bin/env python3
"""Create initial-flash and Ethernet-OTA release artifacts from an IDF build."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

from device_admin import inspect_ota_image


ROOT = Path(__file__).resolve().parents[1]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument(
        "--output-root",
        type=Path,
        default=ROOT / "release",
        help="new version directory is created below this path",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    build_dir = args.build_dir.resolve()
    flasher_path = build_dir / "flasher_args.json"
    try:
        flasher = json.loads(flasher_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read {flasher_path}; run idf.py build first: {error}") from error
    project_description_path = build_dir / "project_description.json"
    try:
        project_description = json.loads(project_description_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read {project_description_path}: {error}") from error

    app_entry = flasher.get("app") or {}
    app_path = build_dir / str(app_entry.get("file", ""))
    if not app_path.is_file():
        raise RuntimeError(f"application image is missing: {app_path}")
    app_info = inspect_ota_image(app_path.read_bytes())
    version = app_info["version"]
    output = (args.output_root / f"v{version}").resolve()
    if output.exists():
        raise RuntimeError(f"refusing to overwrite existing release directory: {output}")
    output.mkdir(parents=True)

    flash_files: dict[str, str] = flasher.get("flash_files", {})
    resolved_flash_files: list[tuple[str, Path]] = []
    for offset, relative in sorted(flash_files.items(), key=lambda item: int(item[0], 0)):
        source = build_dir / relative
        if not source.is_file():
            raise RuntimeError(f"flash input is missing: {source}")
        resolved_flash_files.append((offset, source))

    ota_output = output / "firmware-ota.bin"
    shutil.copy2(app_path, ota_output)
    for _offset, source in resolved_flash_files:
        if source == app_path:
            continue
        shutil.copy2(source, output / source.name)

    settings = flasher.get("flash_settings", {})
    merged_output = output / "initial-flash.bin"
    merge_command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "merge_bin",
        "-o",
        str(merged_output),
        "--flash_mode",
        str(settings.get("flash_mode", "dio")),
        "--flash_size",
        str(settings.get("flash_size", "16MB")),
        "--flash_freq",
        str(settings.get("flash_freq", "80m")),
    ]
    for offset, source in resolved_flash_files:
        merge_command.extend((offset, str(source)))
    try:
        subprocess.run(merge_command, check=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError(
            "esptool merge failed; run this script inside the ESP-IDF environment"
        ) from error

    packaged_flash_files = [
        (offset, ota_output.name if source == app_path else source.name)
        for offset, source in resolved_flash_files
    ]
    flash_args = " ".join(
        [item for offset, name in packaged_flash_files for item in (offset, name)]
    )
    (output / "flash-args.txt").write_text(flash_args + "\n", encoding="utf-8")

    shutil.copy2(ROOT / "LICENSE", output / "LICENSE")
    shutil.copy2(ROOT / "THIRD_PARTY_NOTICES.md", output / "THIRD_PARTY_NOTICES.md")
    shutil.copytree(ROOT / "third_party" / "bacnet-stack" / "license", output / "licenses" / "bacnet-stack")
    idf_path = Path(str(project_description.get("idf_path", "")))
    idf_licenses = {
        "LICENSE": "ESP-IDF/Apache-2.0",
        "components/freertos/FreeRTOS-Kernel/LICENSE.md": "FreeRTOS/LICENSE.md",
        "components/http_parser/LICENSE.txt": "http-parser/LICENSE.txt",
        "components/json/cJSON/LICENSE": "cJSON/LICENSE",
        "components/lwip/lwip/COPYING": "lwIP/COPYING",
        "components/mbedtls/mbedtls/LICENSE": "mbedTLS/LICENSE",
        "components/newlib/COPYING.NEWLIB": "newlib/COPYING.NEWLIB",
        "components/newlib/COPYING.picolibc": "newlib/COPYING.picolibc",
    }
    for source_name, destination_name in idf_licenses.items():
        source = idf_path / source_name
        if not source.is_file():
            raise RuntimeError(f"required ESP-IDF license is missing: {source}")
        destination = output / "licenses" / destination_name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)

    artifacts: dict[str, dict[str, object]] = {}
    for path in sorted(output.rglob("*")):
        if path.is_file() and path.name not in {"manifest.json", "SHA256SUMS"}:
            relative = path.relative_to(output).as_posix()
            artifacts[relative] = {"bytes": path.stat().st_size, "sha256": sha256_file(path)}
    manifest = {
        "target": "ESP32-S3-PoE-ETH-8DI-8RO-C",
        "project": app_info["project"],
        "version": version,
        "idf_version": app_info["idf_version"],
        "ota_partition_bytes": 0x600000,
        "artifacts": artifacts,
    }
    manifest_path = output / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    checksummed = [
        path for path in sorted(output.rglob("*"))
        if path.is_file() and path.name != "SHA256SUMS"
    ]
    (output / "SHA256SUMS").write_text(
        "".join(
            f"{sha256_file(path)}  {path.relative_to(output).as_posix()}\n"
            for path in checksummed
        ),
        encoding="ascii",
    )
    print(output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
