#!/usr/bin/env python3
"""Regression tests for production ESP-IDF configuration defaults."""

from pathlib import Path
import unittest


SDKCONFIG_DEFAULTS = Path(__file__).parents[1] / "sdkconfig.defaults"


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
        self.assertGreaterEqual(int(settings["CONFIG_LWIP_UDP_RECVMBOX_SIZE"]), 32)


if __name__ == "__main__":
    unittest.main()
