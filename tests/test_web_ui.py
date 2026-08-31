#!/usr/bin/env python3
"""Static integrity tests for the embedded management interface."""

from html.parser import HTMLParser
from pathlib import Path
import re
import unittest


WEB_UI = Path(__file__).parents[1] / "main" / "web" / "index.html"


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


if __name__ == "__main__":
    unittest.main()
