#!/usr/bin/env python3
"""Static integrity tests for the embedded management interface."""

from __future__ import annotations

from html.parser import HTMLParser
from pathlib import Path
import re
import unittest


WEB_UI = Path(__file__).parents[1] / "main" / "web" / "index.html"
WEB_ADMIN = Path(__file__).parents[1] / "main" / "web_admin.c"
APP_MAIN = Path(__file__).parents[1] / "main" / "app_main.c"


class IdCollector(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.ids: list[str] = []

    def handle_starttag(self, _tag: str, attrs: list[tuple[str, str | None]]) -> None:
        for name, value in attrs:
            if name == "id" and value is not None and "${" not in value:
                self.ids.append(value)


class WebUiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.page = WEB_UI.read_text(encoding="utf-8")
        cls.web_admin = WEB_ADMIN.read_text(encoding="utf-8")
        cls.app_main = APP_MAIN.read_text(encoding="utf-8")

    def test_page_is_self_contained_and_bounded(self) -> None:
        self.assertLess(len(self.page.encode("utf-8")), 96 * 1024)
        self.assertNotRegex(self.page, r'<(?:script|link)[^>]+(?:src|href)="https?://')
        self.assertIn("SPDX-License-Identifier: Apache-2.0", self.page)

    def test_static_element_ids_are_unique(self) -> None:
        parser = IdCollector()
        parser.feed(self.page)
        duplicates = {value for value in parser.ids if parser.ids.count(value) > 1}
        self.assertEqual(duplicates, set())

    def test_repository_features_are_exposed(self) -> None:
        for label in (
            "Relay Control",
            "BACnet &amp; Status",
            "Configuration",
            "Firmware Update",
            "Digital inputs",
            "Relay outputs",
        ):
            self.assertIn(label, self.page)
        for path in (
            "/api/v1/status",
            "/api/v1/config",
            "/api/v1/auth/challenge",
            "/api/v1/relay",
            "/api/v1/ota",
            "/api/v1/reboot",
        ):
            self.assertIn(path, self.page)

    def test_commands_use_hmac_and_bacnet_priority(self) -> None:
        self.assertIn("BACNET-IO-AUTH-V1", self.page)
        self.assertIn("hmacSha256", self.page)
        self.assertIn("cryptoSelfTest", self.page)
        self.assertIn("priority === 6", self.page)
        self.assertNotIn("'/Switch", self.page)
        self.assertNotIn("'/AllOn", self.page)

    def test_no_browser_storage_for_admin_key(self) -> None:
        self.assertNotRegex(self.page, re.compile(r"\b(?:localStorage|sessionStorage|indexedDB)\b"))

    def test_reset_diagnostics_are_exposed(self) -> None:
        self.assertIn('id="sReset"', self.page)
        self.assertIn("status.last_reset_reason", self.page)
        self.assertIn('"last_reset_reason"', self.web_admin)
        self.assertIn('"last_reset_reason_code"', self.web_admin)
        self.assertIn("ESP-IDF reset reason code", self.app_main)
        for reason in ("power-on", "software", "panic", "task-watchdog", "brownout"):
            self.assertIn(f'"{reason}"', self.web_admin)

    def test_relay_validation_errors_are_specific(self) -> None:
        for message in (
            "channel must be between 1 and %u",
            "priority must be between 1 and 16",
            "priority 6 is reserved",
            "state must be on, off, or relinquish",
        ):
            self.assertIn(message, self.web_admin)
        self.assertNotIn(
            "state must be on, off, or relinquish; priority 6 is reserved",
            self.web_admin,
        )


if __name__ == "__main__":
    unittest.main()
