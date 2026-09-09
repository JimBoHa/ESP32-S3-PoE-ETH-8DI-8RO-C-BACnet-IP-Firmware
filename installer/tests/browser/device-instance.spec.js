import { test, expect } from "@playwright/test";
import { readFile } from "node:fs/promises";

const pageSource = await readFile(new URL("../../../main/web/index.html", import.meta.url), "utf8");
const exampleKey = "cd".repeat(32);

async function devicePage(page, overrides = {}) {
  const config = {
    device_instance: 599155, device_instance_auto: true, device_instance_locked: true,
    database_revision: 2, bacnet_port: 47808, vendor_id: 260, vendor_name: "Example vendor",
    hostname: "example-controller", device_name: "BACnet IO 599155", location: "Example room",
    dhcp_enabled: true, restore_relay_state: false, input_invert_mask: 255,
    ip_address: "192.0.2.155", netmask: "255.255.255.0", gateway: "192.0.2.1", dns_server: "192.0.2.1",
    input_names: Array.from({ length: 8 }, (_, i) => `Digital Input ${i + 1}`),
    relay_names: Array.from({ length: 8 }, (_, i) => `Relay Output ${i + 1}`),
    ...overrides,
  };
  const status = {
    ip_address: "192.0.2.155", firmware_version: "1.1.0", bacnet_running: true,
    bacnet_device_instance: 599155, bacnet_instance_status: "locked", bacnet_udp_port: 47808,
    bacnet_udp_receive_mailbox_size: 64, bacnet_packets_received: 12, ethernet_link: true,
    ipv4_assigned: true, relay_controller_healthy: true, rtc_present: true,
    uptime_seconds: 120, free_heap_bytes: 250000, minimum_free_heap_bytes: 240000,
    reboot_count: 2, product: "Example BACnet controller", build_date: "Example build", running_partition: "ota_1",
  };
  const writes = [];
  await page.route("**/api/v1/**", (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    if (request.method() === "PUT") {
      expect(request.headers()["x-authorization"]).toMatch(/^[a-f0-9]{64}$/);
      writes.push(JSON.parse(request.postData()));
      return route.fulfill({ json: { saved: true, reboot_required: true } });
    }
    return route.fulfill({ json: path.endsWith("/config") ? config :
      path.endsWith("/challenge") ? { nonce: "ab".repeat(16) } : status });
  });
  await page.route("**/device-instance-test", (route) => route.fulfill({ contentType: "text/html", body: pageSource }));
  page.on("dialog", (dialog) => dialog.accept());
  await page.goto("/device-instance-test");
  await expect(page.locator("#cDeviceInstance")).toHaveValue("599155");
  await page.locator("#adminKey").fill(exampleKey);
  await page.locator('[data-panel="config"]').click();
  return { config, status, writes };
}

test("automatically assigned instance stays locked and unrelated saves preserve it", async ({ page }) => {
  const { writes } = await devicePage(page);
  await expect(page.locator("#cInstanceAuto")).toBeChecked();
  await expect(page.locator("#cDeviceInstance")).toBeDisabled();
  if (process.env.UPDATE_INSTANCE_SCREENSHOT === "1") {
    await page.setViewportSize({ width: 1200, height: 1450 });
    await page.evaluate(() => {
      for (const [id, text] of [["cInstanceAuto", "1. Automatic selection locks after assignment"],
        ["cDeviceInstance", "2. Uncheck automatic mode to edit this number"],
        ["cNewInstance", "3. Request another assignment only when needed"]]) {
        const control = document.getElementById(id);
        const target = control.closest("label") || control.parentElement;
        target.style.outline = "3px solid #d44b00";
        target.style.outlineOffset = "4px";
        const note = document.createElement("strong");
        note.textContent = text;
        note.style.cssText = "display:block;color:#963000;background:#fff3df;padding:6px;font-size:13px";
        target.append(note);
      }
    });
    await page.locator("#config").screenshot({ path: "../docs/images/firmware-instance-assignment.png" });
  }
  await page.locator("#cLocation").fill("New room");
  await page.locator("#saveConfig").click();
  await expect.poll(() => writes.length).toBe(1);
  expect(writes[0]).toMatchObject({ device_instance_auto: true, device_instance_reselect: false, location: "New room" });
  expect(writes[0]).not.toHaveProperty("device_instance");
});

test("manual override accepts instances outside the automatic range", async ({ page }) => {
  const { writes } = await devicePage(page);
  await page.locator("#cInstanceAuto").uncheck();
  await expect(page.locator("#cDeviceInstance")).toBeEnabled();
  await page.locator("#cDeviceInstance").fill("123456");
  await page.locator("#saveConfig").click();
  await expect.poll(() => writes.length).toBe(1);
  expect(writes[0]).toMatchObject({ device_instance_auto: false, device_instance: 123456, device_instance_reselect: false });
});

test("requesting a fresh automatic assignment is explicit and does not reboot from the form", async ({ page }) => {
  const { writes } = await devicePage(page);
  await page.locator("#cNewInstance").click();
  await expect(page.locator("#cInstanceNote")).toContainText("Update any existing BAS mapping");
  await page.locator("#saveConfig").click();
  await expect.poll(() => writes.length).toBe(1);
  expect(writes[0]).toMatchObject({ device_instance_auto: true, device_instance_reselect: true });
  expect(writes[0]).not.toHaveProperty("device_instance");
});

test("duplicate warning retains the locked ID", async ({ page }) => {
  const { status } = await devicePage(page);
  status.bacnet_instance_status = "locked-conflict";
  await page.locator('[data-panel="system"]').click();
  await expect(page.locator("#sInstanceStatus")).toHaveText("Duplicate detected — instance stays locked");
  await expect(page.locator("#sInstance")).toHaveText("599155");
});

test("completion preserves unsaved user edits", async ({ page }) => {
  const { config, status } = await devicePage(page, { device_instance_locked: false });
  status.bacnet_instance_status = "discovering";
  await page.locator("#cLocation").fill("Unsaved room");
  config.device_instance = 599156;
  config.device_instance_locked = true;
  status.bacnet_device_instance = 599156;
  status.bacnet_instance_status = "locked";
  await expect(page.locator("#sInstance")).toHaveText("599156");
  await expect(page.locator("#cLocation")).toHaveValue("Unsaved room");
});

test("completion refreshes an untouched form even if a later duplicate is already detected", async ({ page }) => {
  const { config, status } = await devicePage(page, { device_instance_locked: false });
  config.device_instance = 599156;
  config.device_instance_locked = true;
  status.bacnet_device_instance = 599156;
  status.bacnet_instance_status = "locked-conflict";
  await expect(page.locator("#cDeviceInstance")).toHaveValue("599156");
  await expect(page.locator("#cInstanceNote")).toContainText("locked");
});

test("switching manual mode to automatic explains that assignment needs a save and reboot", async ({ page }) => {
  await devicePage(page, { device_instance_auto: false });
  await page.locator("#cInstanceAuto").check();
  await expect(page.locator("#cInstanceNote")).toContainText("after Save and Reboot");
});
