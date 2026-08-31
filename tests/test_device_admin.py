#!/usr/bin/env python3
"""Host tests for the Ethernet management client."""

import hashlib
import hmac
from pathlib import Path
import struct
import tempfile
import unittest

from tools.device_admin import (
    APP_DESC_MAGIC,
    APP_DESC_OFFSET,
    AdminError,
    EXPECTED_PROJECT,
    canonical_request,
    inspect_ota_image,
    load_admin_key,
    normalize_device,
)


class DeviceAdminTests(unittest.TestCase):
    def test_canonical_request_and_signature(self) -> None:
        body = b'{"dhcp_enabled":true}'
        canonical = canonical_request(
            "PUT",
            "/api/v1/config",
            "00112233445566778899aabbccddeeff",
            body,
        )
        self.assertEqual(
            canonical,
            b"BACNET-IO-AUTH-V1\nPUT\n/api/v1/config\n"
            b"00112233445566778899aabbccddeeff\n21\n"
            + hashlib.sha256(body).hexdigest().encode("ascii")
            + b"\n",
        )
        self.assertEqual(
            hmac.new(bytes(range(32)), canonical, hashlib.sha256).hexdigest(),
            "f9793b893bc71354610a51200698af5d0048f7c4146289bdbc75b8ca513193fa",
        )

    def test_normalize_device(self) -> None:
        self.assertEqual(normalize_device("192.168.75.153"), "http://192.168.75.153")
        self.assertEqual(normalize_device("http://example.test:8080/"), "http://example.test:8080")
        with self.assertRaises(AdminError):
            normalize_device("http://example.test/api")

    def test_key_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "device.key"
            path.write_text("ab" * 32 + "\n", encoding="ascii")
            path.chmod(0o600)
            self.assertEqual(load_admin_key(path), bytes.fromhex("ab" * 32))
            path.write_text("not-a-key\n", encoding="ascii")
            with self.assertRaises(AdminError):
                load_admin_key(path)

    def test_inspect_ota_image(self) -> None:
        image = bytearray(APP_DESC_OFFSET + 256)
        image[0] = 0xE9
        struct.pack_into("<I", image, APP_DESC_OFFSET, APP_DESC_MAGIC)
        image[APP_DESC_OFFSET + 16 : APP_DESC_OFFSET + 21] = b"0.1.0"
        project = EXPECTED_PROJECT.encode("ascii")
        image[APP_DESC_OFFSET + 48 : APP_DESC_OFFSET + 48 + len(project)] = project
        image[APP_DESC_OFFSET + 112 : APP_DESC_OFFSET + 117] = b"v5.5.4"
        info = inspect_ota_image(bytes(image))
        self.assertEqual(info["project"], EXPECTED_PROJECT)
        self.assertEqual(info["version"], "0.1.0")

    def test_reject_wrong_ota_project(self) -> None:
        image = bytearray(APP_DESC_OFFSET + 256)
        image[0] = 0xE9
        struct.pack_into("<I", image, APP_DESC_OFFSET, APP_DESC_MAGIC)
        image[APP_DESC_OFFSET + 48 : APP_DESC_OFFSET + 53] = b"other"
        with self.assertRaises(AdminError):
            inspect_ota_image(bytes(image))


if __name__ == "__main__":
    unittest.main()
