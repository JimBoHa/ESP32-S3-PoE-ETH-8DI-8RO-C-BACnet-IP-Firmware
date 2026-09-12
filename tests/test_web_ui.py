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
        self.assertIn("SPDX-License-Identifier: 0BSD", self.page)

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
            "/api/v1/time",
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

    def test_udp_receive_capacity_is_exposed(self) -> None:
        self.assertIn('id="sUdpMailbox"', self.page)
        self.assertIn("status.bacnet_udp_receive_mailbox_size", self.page)
        self.assertIn('"bacnet_udp_receive_mailbox_size"', self.web_admin)
        self.assertIn("CONFIG_LWIP_UDP_RECVMBOX_SIZE", self.app_main)
        self.assertIn("_Static_assert", self.app_main)

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

    def test_cov_recovery_status_is_exposed_with_unavailable_fallback(self) -> None:
        self.assertIn("bacnet_app_cov_recovery_get(&cov_stats)", self.web_admin)
        self.assertIn('cJSON_AddObjectToObject(root, "bacnet_cov_recovery")', self.web_admin)
        self.assertIn('cJSON_AddNullToObject(root, "bacnet_cov_recovery")', self.web_admin)
        for field in ("confirmed_timeouts", "refresh_requests", "pending_objects", "capacity_errors"):
            self.assertIn(f'"{field}", cov_stats.{field}', self.web_admin)

    def test_relay_driver_readback_and_recovery_diagnostics_are_exposed(self) -> None:
        self.assertIn("board_io_relay_diagnostics_get(&relay_diagnostics)", self.web_admin)
        self.assertIn('cJSON_AddObjectToObject(root, "relay_driver")', self.web_admin)
        for field in ("desired_mask", "applied_mask", "healthy", "registers_valid",
                      "output_register", "configuration_register", "i2c_errors",
                      "verification_failures", "configuration_recoveries", "mutex_timeouts", "last_error"):
            self.assertIn(f'"{field}", relay_diagnostics.{field}', self.web_admin)
        for field in ("output_register", "configuration_register"):
            self.assertIn(f'cJSON_AddNullToObject(relay, "{field}")', self.web_admin)
        self.assertIn('"last_verified_ms", (double)relay_diagnostics.last_verified_ms', self.web_admin)

    def test_time_configuration_is_separate_authenticated_and_live_applied(self) -> None:
        self.assertIn('id="timeForm"', self.page)
        self.assertIn("authFetch('/api/v1/time', 'PUT'", self.page)
        self.assertIn('request_authorize(request, "PUT", "/api/v1/time"', self.web_admin)
        self.assertIn("TIME_BODY_MAX 1024U", self.web_admin)
        handler = self.web_admin.split("static esp_err_t time_put_handler", 1)[1].split(
            "static esp_err_t relay_put_handler", 1)[0]
        self.assertLess(handler.index("request_authorize("), handler.index("cJSON_ParseWithLengthOpts("))
        self.assertLess(handler.index("time_config_update(&candidate)"), handler.index("clock_service_config_changed()"))
        self.assertIn("server_fields == 1U && timezone_fields == 1U", handler)
        self.assertIn("body_length + 1U, NULL, true", handler)
        self.assertIn("embedded_null", handler)
        self.assertNotIn("config_store_update", handler)
        self.assertNotIn("delayed_restart", handler)
        self.assertIn('config.max_uri_handlers = 11;', self.web_admin)
        self.assertIn('config.stack_size = 8192;', self.web_admin)

    def test_time_presets_and_dirty_refresh_are_explicit(self) -> None:
        self.assertIn('value="PST8PDT,M3.2.0/2,M11.1.0/2">America/Los_Angeles', self.page)
        self.assertIn('value="UTC0">UTC', self.page)
        self.assertIn('value="custom">Custom POSIX TZ rule', self.page)
        self.assertIn("renderTimeConfig(timeConfig, !timeDirty)", self.page)
        self.assertIn("if (timeEditVersion === editVersion) renderTimeConfig(latestTimeConfig, true)", self.page)
        self.assertIn("if (!fillForm) return;", self.page.split("function renderTimeConfig", 1)[1])
        self.assertIn("NTP is unauthenticated", self.page)
        self.assertIn("not a downloadable timezone database", self.page)

    def test_time_and_startup_status_distinguish_validity_from_transport_acceptance(self) -> None:
        self.assertIn("clock_service_status_get(&status)", self.web_admin)
        for field in ("valid", "synchronized", "source", "sync_count", "rejected_syncs",
                      "start_failures", "config_generation"):
            self.assertIn(f'"{field}", status.{field}', self.web_admin)
        self.assertIn("bacnet_app_time_stats_get(&time_stats)", self.web_admin)
        self.assertIn("bacnet_app_announcement_stats_get(&announcement_stats)", self.web_admin)
        self.assertIn('"boot_destinations"', self.web_admin)
        self.assertIn('"timestamp_from_valid_clock", time_stats.timestamp_from_valid_clock', self.web_admin)
        self.assertIn("local transport acceptance; not remote receipt or command restoration", self.web_admin)
        self.assertIn("status.bacnet_announcement.transport_acceptances", self.page)
        self.assertIn("time?.valid ? time.local_time", self.page)


if __name__ == "__main__":
    unittest.main()
