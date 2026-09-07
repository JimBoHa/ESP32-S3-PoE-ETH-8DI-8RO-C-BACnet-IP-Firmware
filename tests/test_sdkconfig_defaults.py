#!/usr/bin/env python3
"""Regression tests for production ESP-IDF configuration defaults."""

from pathlib import Path
import re
import unittest


SDKCONFIG_DEFAULTS = Path(__file__).parents[1] / "sdkconfig.defaults"
FIRMWARE_HEADER = Path(__file__).parents[1] / "main" / "firmware.h"
APP_MAIN = Path(__file__).parents[1] / "main" / "app_main.c"


class SdkconfigDefaultsTests(unittest.TestCase):
    def test_udp_receive_mailbox_handles_bacnet_point_scan_bursts(self) -> None:
        settings: dict[str, str] = {}
        for raw_line in SDKCONFIG_DEFAULTS.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, value = line.split("=", 1)
            settings[name] = value

        self.assertIn("CONFIG_LWIP_UDP_RECVMBOX_SIZE", settings)
        self.assertEqual(int(settings["CONFIG_LWIP_UDP_RECVMBOX_SIZE"]), 64)

        header = FIRMWARE_HEADER.read_text(encoding="utf-8")
        match = re.search(
            r"#define FW_MIN_BACNET_UDP_RECEIVE_MAILBOX_SIZE (\d+)U", header
        )
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)), 64)
        app_main = APP_MAIN.read_text(encoding="utf-8")
        self.assertIn("_Static_assert(CONFIG_LWIP_UDP_RECVMBOX_SIZE", app_main)
        self.assertIn("FW_MIN_BACNET_UDP_RECEIVE_MAILBOX_SIZE", app_main)


if __name__ == "__main__":
    unittest.main()
