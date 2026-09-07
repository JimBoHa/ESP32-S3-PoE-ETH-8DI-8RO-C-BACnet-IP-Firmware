// SPDX-License-Identifier: Apache-2.0
// Documentation-only rendering of the real interfaces with deterministic example
// data. No serial device or live controller is contacted by this script.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { chromium } from "@playwright/test";
import { createServer } from "vite";

const root = fileURLToPath(new URL("../", import.meta.url));
const output = fileURLToPath(new URL("../../docs/images/", import.meta.url));
const origin = "http://127.0.0.1:5180";
const exampleKey = "ab".repeat(32);
const version = "0.14.0";
const project = "esp32_s3_poe_eth_8di_8ro_bacnet";
const image = Buffer.alloc(300);
image[0] = 0xe9; image[3] = 0x4f; image[12] = 9;
const asset = { path: `${version}/initial-flash.bin`, bytes: image.length,
  sha256: createHash("sha256").update(image).digest("hex") };
const catalog = { schema: 1, recommended: version, releases: [{ version,
  project, chipFamily: "ESP32-S3", flashBytes: 16777216, usbSetupProtocol: 1,
  initial: asset, ota: { ...asset, path: `${version}/firmware-ota.bin` } }] };

const config = {
  device_instance: 123456, bacnet_port: 47808, vendor_id: 260,
  device_name: "Demo I/O Controller", hostname: "bacnet-io-123456",
  vendor_name: "Waveshare", location: "Example plant room", database_revision: 1,
  input_invert_mask: 0, dhcp_enabled: true, restore_relay_state: false,
  ip_address: "192.0.2.140", netmask: "255.255.255.0", gateway: "192.0.2.1", dns_server: "192.0.2.1",
  input_names: Array.from({ length: 8 }, (_, i) => `Digital Input ${i + 1}`),
  relay_names: Array.from({ length: 8 }, (_, i) => `Relay Output ${i + 1}`),
};
const status = {
  product: "ESP32-S3-PoE-ETH-8DI-8RO-C BACnet/IP", firmware_version: version,
  build_date: "Sep 7 2026", running_partition: "ota_0", uptime_seconds: 3661,
  reboot_count: 1, last_reset_reason: "power-on", last_reset_reason_code: 1,
  ip_address: config.ip_address, ethernet_link: true, ipv4_assigned: true,
  bacnet_running: true, bacnet_device_instance: config.device_instance,
  bacnet_udp_port: 47808, bacnet_udp_receive_mailbox_size: 64,
  bacnet_packets_received: 2400, digital_inputs_mask: 5, relay_outputs_mask: 2,
  relay_commands_mask: 2, relay_active_priorities: [0, 8, 0, 0, 0, 0, 0, 0],
  relay_controller_healthy: true, rtc_present: true,
  free_heap_bytes: 260000, minimum_free_heap_bytes: 255000,
};
const usbStatus = { ...status, ok: true, protocol: 1, project,
  ethernet_mac: "02:00:00:12:34:56", hostname: config.hostname,
  device_instance: config.device_instance, key_export_allowed: true };

await mkdir(output, { recursive: true });
const server = await createServer({ root, server: { host: "127.0.0.1", port: 5180, strictPort: true } });
await server.listen();
const browser = await chromium.launch();
const context = await browser.newContext({ viewport: { width: 1440, height: 1080 },
  deviceScaleFactor: 1, locale: "en-US", timezoneId: "UTC", colorScheme: "light" });
const captures = [];

async function capture(page, name, selector) {
  await page.evaluate(() => document.fonts.ready);
  // Native progress controls can finish painting after DOM layout settles.
  await page.waitForTimeout(250);
  assert(!(await page.locator("body").innerText()).includes(exampleKey));
  const target = selector ? page.locator(selector) : page;
  await target.screenshot({ path: `${output}/${name}.png`, animations: "disabled",
    ...(selector ? {} : { fullPage: true }) });
  captures.push(`${name}.png`);
}

try {
  // Block every external request, including accidental navigation to the sample
  // device address. Only this temporary localhost server can receive traffic.
  await context.route("**/*", (route) => new URL(route.request().url()).origin === origin
    ? route.continue() : route.abort());
  const installer = await context.newPage();
  await installer.addInitScript(() => {
    Object.defineProperty(navigator, "serial", { configurable: true,
      value: { requestPort: async () => ({}) } });
  });
  await installer.route("**/firmware/catalog.json", (route) => route.fulfill({ json: catalog }));
  await installer.route("**/firmware/0.14.0/initial-flash.bin", (route) => route.fulfill({ body: image }));
  await installer.route("**/src/device.js", (route) => route.fulfill({ contentType: "application/javascript", body: `
    const status = ${JSON.stringify(usbStatus)};
    export class Controller {
      constructor(port, progress) { this.progress = progress; this.identity = { chip: "ESP32-S3", mac: "02:00:00:12:34:50" }; }
      async connect() { return this.identity; }
      async backup() { return new Uint8Array(16777216); }
      async install() { this.progress("write", 0.57); await new Promise(resolve => { window.finishDocumentationFlash = resolve; }); this.progress("verified", 1); }
      async startApplication() { this.setup = true; return status; }
      async status() { return status; }
      async key() { return "${exampleKey}"; }
      async lock() { status.key_export_allowed = false; }
      async disconnect() { this.setup = false; }
    }
  ` }));
  await installer.goto(origin);
  await installer.locator("#version option").filter({ hasText: "Recommended" }).waitFor({ state: "attached" });
  await capture(installer, "install-01-connect");
  await installer.getByRole("button", { name: "Connect controller", exact: true }).click();
  await installer.locator("#board-confirm").check();
  await installer.locator("#erase-confirm").check();
  await capture(installer, "install-02-confirm");
  await installer.getByRole("button", { name: "Erase & install firmware", exact: true }).click();
  await installer.getByText("Writing firmware", { exact: true }).waitFor();
  await capture(installer, "install-03-writing");
  await installer.evaluate(() => window.finishDocumentationFlash());
  await installer.getByRole("heading", { name: "Firmware installed", exact: true }).waitFor();
  await capture(installer, "install-04-key-network", "#setup-options");
  const downloaded = installer.waitForEvent("download");
  await installer.getByRole("button", { name: "Download admin key", exact: true }).click();
  await (await downloaded).delete();
  await installer.getByRole("button", { name: "Finish USB setup", exact: true }).click();
  await installer.getByRole("heading", { name: "USB setup complete", exact: true }).waitFor();
  await capture(installer, "install-05-complete");

  const device = await context.newPage();
  const source = await readFile(new URL("../../main/web/index.html", import.meta.url), "utf8");
  await device.route("**/device", (route) => route.fulfill({ contentType: "text/html", body: source }));
  await device.route("**/api/v1/status", (route) => route.fulfill({ json: status }));
  await device.route("**/api/v1/config", (route) => route.fulfill({ json: config }));
  await device.goto(`${origin}/device`);
  await device.getByText("192.0.2.140 · firmware 0.14.0 · BACnet/IP online", { exact: true }).waitFor();
  await device.locator("#keyFile").setInputFiles({ name: "bacnet-io-example-admin.key", mimeType: "text/plain", buffer: Buffer.from(`${exampleKey}\n`) });
  await device.getByText("Admin key loaded locally. The file was not uploaded.", { exact: true }).waitFor();
  await capture(device, "install-06-load-key", ".toolbar");
  // Let the transient key-loaded toast disappear before overview screenshots.
  await device.waitForFunction(() => !document.getElementById("toast").classList.contains("show"));
  await capture(device, "firmware-relays");
  await device.getByRole("button", { name: "BACnet & Status", exact: true }).click();
  await capture(device, "firmware-status");
  await device.getByRole("button", { name: "Configuration", exact: true }).click();
  await capture(device, "firmware-configuration");
  await device.getByRole("button", { name: "Firmware", exact: true }).click();
  await capture(device, "firmware-update");
  await writeFile(`${output}/captures.json`, JSON.stringify({ firmware: version,
    browser: browser.version(), viewport: "1440 x 1080", source: "Actual application interfaces rendered with isolated example data; no live hardware.",
    files: captures }, null, 2) + "\n");
  console.log(`Saved ${captures.length} documentation screenshots to docs/images.`);
} finally {
  await context.close();
  await browser.close();
  await server.close();
}
