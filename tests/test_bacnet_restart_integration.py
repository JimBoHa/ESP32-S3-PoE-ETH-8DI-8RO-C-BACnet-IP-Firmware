#!/usr/bin/env python3
"""Static restart wiring regressions; these do not execute firmware or networking.

The companion C harness exercises codecs/scheduler behavior. These checks guard
the actual ESP-IDF source selection, permissions and application startup wiring.
"""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function_body(source: str, name: str) -> str:
    """Find a C definition and balance braces, ignoring comments and strings."""
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", source)
    if not match:
        raise AssertionError(f"C function definition missing: {name}")
    start = match.end()
    masked = re.sub(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda token: " " * len(token.group()), source[start:], flags=re.S,
    )
    depth = 1
    for offset, char in enumerate(masked):
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:start + offset]
    raise AssertionError(f"Unbalanced C function: {name}")


def compact(source: str) -> str:
    return re.sub(r"\s+", " ", source).strip()


class BacnetRestartIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = (ROOT / "main/bacnet_app.c").read_text(encoding="utf-8")
        cls.main = (ROOT / "main/app_main.c").read_text(encoding="utf-8")
        cls.web = (ROOT / "main/web_admin.c").read_text(encoding="utf-8")
        cls.component = (ROOT / "components/bacnet_stack/CMakeLists.txt").read_text(encoding="utf-8")

    def test_actual_pinned_server_and_restart_source_are_compiled(self) -> None:
        self.assertIn('"${BACNET_SRC}/bacnet/basic/server/bacnet_device.c"', self.component)
        self.assertNotIn('"${BACNET_SRC}/bacnet/basic/object/device.c"', self.component)
        self.assertIn('"${BACNET_SRC}/bacnet/basic/sys/datetime_mstimer.c"', self.component)
        self.assertIn('"bacnet_restart.c"', (ROOT / "main/CMakeLists.txt").read_text())
        server = (ROOT / "third_party/bacnet-stack/src/bacnet/basic/server/bacnet_device.c").read_text()
        for property_name in ("PROP_LOCAL_DATE", "PROP_LOCAL_TIME", "PROP_TIME_OF_DEVICE_RESTART"):
            self.assertIn(property_name, server)

    def test_device_table_uses_narrow_wrappers(self) -> None:
        table = self.app[self.app.index("static object_functions_t s_object_table[]"):]
        entry = re.search(r"\{\s*OBJECT_DEVICE\b([^{}]*)\}", table)
        self.assertIsNotNone(entry)
        body = compact(entry.group(1))
        self.assertIn("device_read_property, device_write_property, device_property_lists", body)
        self.assertIn("bacnet_restart_writable_property_list", body)
        self.assertNotIn("Device_Write_Property_Local", body)

    def test_device_write_wrapper_does_not_open_other_device_writes(self) -> None:
        body = compact(function_body(self.app, "device_write_property"))
        self.assertIn("if (!data) { return false; }", body)
        self.assertIn("if (data->object_property == PROP_RESTART_NOTIFICATION_RECIPIENTS) { return bacnet_restart_write_property(data); }", body)
        self.assertIn("Device_Objects_Property_List_Member(OBJECT_DEVICE", body)
        self.assertIn("ERROR_CODE_WRITE_ACCESS_DENIED : ERROR_CODE_UNKNOWN_PROPERTY", body)
        self.assertTrue(body.endswith("return false;"))
        self.assertNotIn("Device_Write_Property_Local", body)
        self.assertNotIn("Binary_Output", body)

    def test_read_wrapper_preserves_upstream_device_reads(self) -> None:
        body = compact(function_body(self.app, "device_read_property"))
        self.assertIn("data->object_property == PROP_RESTART_NOTIFICATION_RECIPIENTS", body)
        self.assertIn("return bacnet_restart_read_property(data);", body)
        self.assertTrue(body.endswith("return Device_Read_Property_Local(data);"))

    def test_property_lists_preserve_upstream_and_append_one_optional(self) -> None:
        wrapper = compact(function_body(self.app, "device_property_lists"))
        self.assertIn("Device_Property_Lists(required, NULL, proprietary);", wrapper)
        self.assertIn("*optional = s_device_optional_properties;", wrapper)
        initializer = compact(function_body(self.app, "initialize_device_property_list"))
        self.assertIn("Device_Property_Lists(NULL, &optional, NULL);", initializer)
        self.assertIn("s_device_optional_properties[count] = optional[count];", initializer)
        self.assertIn("sizeof(s_device_optional_properties)", initializer)
        self.assertIn("if (!present)", initializer)
        self.assertIn("s_device_optional_properties[count++] = PROP_RESTART_NOTIFICATION_RECIPIENTS;", initializer)
        self.assertIn("s_device_optional_properties[count] = -1;", initializer)

    def test_startup_gate_follows_services_and_ota_validation(self) -> None:
        body = function_body(self.main, "app_main")
        complete = body.index("bacnet_app_startup_complete();")
        for prerequisite in ("bacnet_app_start(&config)", "web_admin_start()",
                             "usb_setup_start(key_created)", "esp_ota_mark_app_valid_cancel_rollback()"):
            self.assertLess(body.index(prerequisite), complete)
        self.assertEqual(body.count("bacnet_app_startup_complete();"), 1)
        self.assertRegex(body, re.compile(r"if\s*\(pending_verify\)\s*\{[^{}]*esp_ota_mark_app_valid_cancel_rollback\(\)[^{}]*\}\s*bacnet_app_startup_complete\(\);", re.S))
        setter = compact(function_body(self.app, "bacnet_app_startup_complete"))
        self.assertLess(setter.index("xSemaphoreTake(s_object_mutex"), setter.index("s_startup_complete = true;"))
        self.assertIn("xSemaphoreGive(s_object_mutex);", setter)

    def test_tick_requires_completed_startup_and_ready_network_identity(self) -> None:
        body = compact(function_body(self.app, "bacnet_task"))
        self.assertIn("while (ethernet_manager_has_ip() && ethernet_manager_network_revision() == revision)", body)
        self.assertIn("bool startup_ready = s_startup_complete && s_running && s_instance.state == INSTANCE_READY;", body)
        self.assertIn("bacnet_announcement_tick(&s_announcement, now_ms, startup_ready, startup_announcement_send, NULL);", body)
        self.assertIn("bacnet_restart_tick(now_ms, startup_ready, Device_Object_Instance_Number(), Device_System_Status());", body)
        self.assertLess(body.index("bacnet_announcement_tick(&s_announcement, now_ms"),
                        body.index("select_restart_timestamp(now_ms, startup_ready);"))
        self.assertLess(body.index("select_restart_timestamp(now_ms, startup_ready);"),
                        body.index("bacnet_restart_tick(now_ms, startup_ready"))
        self.assertNotIn("bacnet_restart_init(", body)
        self.assertNotIn("initialize_restart_notification(", body)
        self.assertEqual(len(re.findall(r"s_startup_complete\s*=\s*true", self.app)), 1)
        self.assertNotRegex(self.app, r"s_startup_complete\s*=\s*false")

    def test_clock_and_frozen_datetime_are_initialized_once(self) -> None:
        body = compact(function_body(self.app, "initialize_restart_notification"))
        self.assertIn("if (!s_clock_snapshot_valid)", body)
        self.assertIn("bacnet_restart_init(state, encoded, length, NULL, restart_reason_to_bacnet()", body)
        fallback = compact(function_body(self.app, "fallback_restart_timestamp"))
        self.assertIn("datetime_set_date(&local.date, 1990, 1, 1);", fallback)
        self.assertIn("bacapp_timestamp_datetime_set(&timestamp, &local);", fallback)
        selection = compact(function_body(self.app, "select_restart_timestamp"))
        self.assertIn("s_clock_snapshot.boot_local_time", selection)
        self.assertIn("s_clock_snapshot.boot_utc_unix_us", selection)
        self.assertIn("bacnet_restart_clock_select(", selection)
        self.assertIn("BACNET_RESTART_CLOCK_WAIT_MS", selection)
        self.assertIn("update_bacnet_clock(now_ms);", selection)
        self.assertIn("bacnet_restart_set_timestamp_before_ready(&timestamp)", selection)
        self.assertIn("Device_Set_Time_Of_Restart(&timestamp)", selection)
        self.assertNotIn("bacapp_timestamp_sequence_set", self.app)
        self.assertEqual(len(re.findall(r"\binitialize_restart_notification\(\s*\)\s*;", self.app)), 1)

    def test_announcement_reports_transport_result_and_wait_does_not_block(self) -> None:
        sender = compact(function_body(self.app, "startup_announcement_send"))
        self.assertIn("dcc_communication_initiation_disabled()", sender)
        self.assertIn("iam_encode_apdu(", sender)
        self.assertIn("return restart_send(&local_broadcast, apdu, (size_t)length, NULL);", sender)
        self.assertNotIn("Send_I_Am(", sender)
        selector = compact(function_body(self.app, "select_restart_timestamp"))
        self.assertNotIn("vTaskDelay", selector)
        self.assertNotIn("while (", selector)
        who_is = compact(function_body(self.app, "handler_who_is_compatible"))
        self.assertIn("!s_startup_complete", who_is)

    def test_time_reads_use_valid_clock_without_opening_writes(self) -> None:
        updater = compact(function_body(self.app, "update_bacnet_clock"))
        self.assertIn("clock_service_snapshot_get(&snapshot)", updater)
        self.assertIn("datetime_timesync(&local.date, &local.time, false);", updater)
        self.assertIn("datetime_utc_offset_minutes_set(snapshot.utc_offset_minutes)", updater)
        self.assertNotIn("Binary_Output", updater)
        wrapper = compact(function_body(self.app, "device_read_property"))
        for name in ("PROP_LOCAL_DATE", "PROP_LOCAL_TIME", "PROP_UTC_OFFSET", "PROP_DAYLIGHT_SAVINGS_STATUS"):
            self.assertIn(name, wrapper)
        self.assertIn("s_clock_snapshot_valid", wrapper)
        self.assertIn("return clock_read_property(data);", wrapper)
        reader = compact(function_body(self.app, "clock_read_property"))
        self.assertIn("s_clock_snapshot.daylight_saving", reader)
        self.assertIn("s_clock_snapshot.utc_offset_minutes", reader)
        self.assertIn("data->application_data_len < length", reader)
        task = compact(function_body(self.app, "bacnet_task"))
        self.assertLess(task.index("update_bacnet_clock((uint64_t)esp_timer_get_time() / 1000U);"),
                        task.index("npdu_handler(&source, s_pdu_buffer, length);"))

    def test_storage_errors_remain_distinct_and_persist_is_checked(self) -> None:
        body = compact(function_body(self.app, "initialize_restart_notification"))
        self.assertIn("loaded == ESP_OK ? BACNET_RESTART_LOAD_VALID : loaded == ESP_ERR_NVS_NOT_FOUND ? BACNET_RESTART_LOAD_MISSING : BACNET_RESTART_LOAD_INVALID", body)
        persist = compact(function_body(self.app, "restart_persist"))
        self.assertIn("return config_store_restart_recipients_set(bytes, length) == ESP_OK;", persist)

    def test_explicit_npdu_sender_preserves_destination_and_bounds(self) -> None:
        body = compact(function_body(self.app, "restart_send"))
        self.assertIn("BACNET_ADDRESS target = *destination", body)
        self.assertIn("npdu_encode_npdu_data(&npdu, false, MESSAGE_PRIORITY_NORMAL);", body)
        self.assertIn("npdu_encode_pdu(pdu, &target, &source, &npdu)", body)
        self.assertIn("length > sizeof(pdu) - (size_t)offset", body)
        self.assertIn("bip_send_pdu(&target, &npdu, pdu", body)
        self.assertIn("destination->net != 0", body)
        self.assertIn("destination->mac_len != 6", body)
        self.assertNotIn("Send_UCOV_Notify", body)
        self.assertNotIn("ucov_notify_encode_pdu", body)
        self.assertNotIn("bip_get_broadcast_address", body)

    def test_device_discovery_uses_requested_binding_and_keeps_identity_checks(self) -> None:
        resolver = compact(function_body(self.app, "restart_resolve"))
        self.assertIn("address_bind_request(recipient->type.device.instance", resolver)
        self.assertIn("recipient->type.address.net != 0", resolver)
        discovery = compact(function_body(self.app, "restart_discover"))
        self.assertIn("recipient->tag == BACNET_RECIPIENT_TAG_DEVICE", discovery)
        self.assertIn("Send_WhoIs_Local(recipient->type.device.instance, recipient->type.device.instance);", discovery)
        iam = compact(function_body(self.app, "handler_instance_i_am"))
        self.assertIn("address_add_binding(instance, max_apdu, source);", iam)
        self.assertIn("bacnet_instance_observe(&s_instance", iam)
        self.assertLess(iam.index("bacnet_iam_request_decode("), iam.index("address_add_binding("))
        self.assertNotRegex(iam, r"\baddress_add\(")

    def test_http_stats_use_bounded_locked_snapshots(self) -> None:
        body = compact(function_body(self.app, "bacnet_app_restart_stats_get"))
        self.assertIn("xSemaphoreTake(s_object_mutex, pdMS_TO_TICKS(250)) != pdTRUE", body)
        self.assertLess(body.index("xSemaphoreTake("), body.index("bacnet_restart_stats_get(stats);"))
        self.assertIn("xSemaphoreGive(s_object_mutex);", body)
        self.assertIn('cJSON_AddObjectToObject(root, "bacnet_restart")', self.web)
        self.assertIn('cJSON_AddNullToObject(root, "bacnet_restart")', self.web)
        for field in ("boot_ready", "recipients_count", "pending_recipients", "notifications_sent",
                      "send_failures", "resolution_failures", "exhausted_recipients",
                      "configuration_errors", "persistence_failures"):
            self.assertIn(f'"{field}", restart_stats.{field}', self.web)
        for getter in ("bacnet_app_announcement_stats_get", "bacnet_app_time_stats_get"):
            body = compact(function_body(self.app, getter))
            self.assertIn("xSemaphoreTake(s_object_mutex, pdMS_TO_TICKS(250)) != pdTRUE", body)
            self.assertIn("xSemaphoreGive(s_object_mutex);", body)


if __name__ == "__main__":
    unittest.main()
