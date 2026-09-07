"""Release-integrity and immutable-catalog checks for the browser installer."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from tools.device_admin import APP_DESC_OFFSET, APP_DESC_MAGIC, EXPECTED_PROJECT
from tools.prepare_installer import stage, verify_package


def package(root: Path, version="0.14.0") -> Path:
    directory = root / version
    directory.mkdir()
    app = bytearray(512)
    app[0] = 0xE9
    app[3] = 0x4F
    app[12] = 9
    struct.pack_into("<I", app, APP_DESC_OFFSET, APP_DESC_MAGIC)
    for offset, text in [(16, version), (48, EXPECTED_PROJECT), (112, "v5.5.4")]:
        data = text.encode()
        app[APP_DESC_OFFSET + offset:APP_DESC_OFFSET + offset + len(data)] = data
    initial = bytearray(0x20000) + app
    initial[0] = 0xE9
    initial[3] = 0x4F
    initial[12] = 9
    (directory / "firmware-ota.bin").write_bytes(app)
    (directory / "initial-flash.bin").write_bytes(initial)
    (directory / "manifest.json").write_text(json.dumps({"version": version,
        "project": EXPECTED_PROJECT, "target": "ESP32-S3-PoE-ETH-8DI-8RO-C"}))
    checksum(directory)
    return directory


def checksum(directory: Path):
    (directory / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n"
        for p in sorted(directory.iterdir()) if p.name != "SHA256SUMS"))


class PrepareInstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_stages_multiple_versions_with_correct_recommendation_and_hash(self):
        first = package(self.root)
        second = package(self.root, "0.14.1")
        result = stage([first, second], self.root / "output", "0.14.1")
        self.assertEqual(result["releases"][0]["version"], "0.14.1")
        self.assertEqual(result["recommended"], "0.14.1")
        image = result["releases"][0]["initial"]
        self.assertEqual(image["sha256"], hashlib.sha256((second / "initial-flash.bin").read_bytes()).hexdigest())

    def test_corrupt_asset_does_not_replace_catalog(self):
        source = package(self.root)
        output = self.root / "output"
        stage([source], output, "0.14.0")
        original = (output / "catalog.json").read_bytes()
        (source / "initial-flash.bin").write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "checksum"):
            stage([source], output, "0.14.0")
        self.assertEqual((output / "catalog.json").read_bytes(), original)

    def test_existing_version_cannot_be_replaced(self):
        source = package(self.root)
        output = self.root / "output"
        stage([source], output, "0.14.0")
        data = bytearray((source / "initial-flash.bin").read_bytes())
        data[1000] ^= 1
        (source / "initial-flash.bin").write_bytes(data)
        checksum(source)
        with self.assertRaisesRegex(ValueError, "immutable"):
            stage([source], output, "0.14.0")

    def test_rejects_old_protocol_and_missing_recommended_version(self):
        source = package(self.root, "0.13.5")
        with self.assertRaisesRegex(ValueError, "0.14.0"):
            verify_package(source)
        source = package(self.root)
        with self.assertRaisesRegex(ValueError, "Recommended"):
            stage([source], self.root / "output", "0.14.1")

    def test_rejects_outside_checksum_path(self):
        source = package(self.root)
        outside = self.root / "outside"
        outside.write_text("outside")
        with (source / "SHA256SUMS").open("a") as f:
            f.write(f"{hashlib.sha256(outside.read_bytes()).hexdigest()}  ../outside\n")
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            verify_package(source)

    def test_rejects_other_project_even_with_recomputed_checksums(self):
        source = package(self.root)
        manifest = json.loads((source / "manifest.json").read_text())
        manifest["project"] = "other"
        (source / "manifest.json").write_text(json.dumps(manifest))
        checksum(source)
        with self.assertRaisesRegex(ValueError, "different"):
            verify_package(source)

    def test_rejects_wrong_initial_application_offset(self):
        source = package(self.root)
        (source / "initial-flash.bin").write_bytes((source / "initial-flash.bin").read_bytes() + b"extra")
        checksum(source)
        with self.assertRaisesRegex(ValueError, "0x20000"):
            verify_package(source)
