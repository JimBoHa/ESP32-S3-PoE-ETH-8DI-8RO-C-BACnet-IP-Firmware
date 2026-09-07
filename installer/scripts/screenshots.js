// SPDX-License-Identifier: 0BSD
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
const version = "1.0.0";
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

async function capture(page, name, selector, notes = []) {
  await page.evaluate(() => document.fonts.ready);
  await page.evaluate(() => window.scrollTo(0, 0));
  // Native progress controls can finish painting after DOM layout settles.
  await page.waitForTimeout(250);
  assert(!(await page.locator("body").innerText()).includes(exampleKey));
  // Annotations are DOM/SVG overlays on the actual interface before capture.
  // Source PNGs are never composited, retouched, or filled with invented UI.
  if (notes.length) {
    const box = selector ? await page.locator(selector).boundingBox() : null;
    const area = box || await page.evaluate(() => ({ x: 0, y: 0,
      width: document.documentElement.clientWidth, height: document.body.scrollHeight }));
    await page.evaluate(({ notes, area }) => {
      const ns = "http://www.w3.org/2000/svg";
      const svg = document.createElementNS(ns, "svg");
      svg.id = "documentation-annotations";
      svg.setAttribute("width", String(document.documentElement.scrollWidth));
      svg.setAttribute("height", String(document.documentElement.scrollHeight));
      Object.assign(svg.style, { position: "absolute", left: "0", top: "0", zIndex: "2147483647", pointerEvents: "none" });
      const element = (tag, attrs, text) => {
        const node = document.createElementNS(ns, tag);
        for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, String(value));
        if (text) node.textContent = text;
        svg.append(node); return node;
      };
      for (const [index, note] of notes.entries()) {
        const target = document.querySelector(note.selector).getBoundingClientRect();
        const rect = { x: target.x + scrollX, y: target.y + scrollY, width: target.width, height: target.height };
        const x = area.x + note.x, y = area.y + note.y;
        const endX = note.edge === "left" ? rect.x - 8 : note.edge === "top" ? rect.x + rect.width / 2 : rect.x + rect.width + 8;
        const endY = note.edge === "top" ? rect.y - 8 : rect.y + rect.height / 2;
        const startX = x + (endX < x ? 0 : endX > x + note.width ? note.width : note.width / 2);
        const startY = y + 24;
        element("rect", { x: rect.x - 5, y: rect.y - 5, width: rect.width + 10, height: rect.height + 10,
          rx: 7, fill: "none", stroke: "#e94c00", "stroke-width": 4 });
        element("line", { x1: startX, y1: startY, x2: endX, y2: endY, stroke: "white", "stroke-width": 8 });
        element("line", { x1: startX, y1: startY, x2: endX, y2: endY, stroke: "#e94c00", "stroke-width": 4 });
        const angle = Math.atan2(endY - startY, endX - startX), size = 13;
        element("polygon", { points: `${endX},${endY} ${endX-size*Math.cos(angle-0.48)},${endY-size*Math.sin(angle-0.48)} ${endX-size*Math.cos(angle+0.48)},${endY-size*Math.sin(angle+0.48)}`, fill: "#e94c00" });
        element("rect", { x, y, width: note.width, height: 48, rx: 10, fill: "#fff8ed", stroke: "#e94c00", "stroke-width": 2 });
        element("circle", { cx: x + 24, cy: y + 24, r: 15, fill: "#c53e00" });
        element("text", { x: x + 24, y: y + 30, fill: "white", "text-anchor": "middle", "font-family": "Arial, sans-serif", "font-size": 17, "font-weight": 700 }, String(index + 1));
        element("text", { x: x + 48, y: y + 30, fill: "#7b2900", "font-family": "Arial, sans-serif", "font-size": 17, "font-weight": 700 }, note.text);
      }
      document.body.append(svg);
    }, { notes, area });
  }
  if (selector && notes.length) {
    const box = await page.locator(selector).boundingBox();
    const size = await page.evaluate(() => ({ width: document.documentElement.scrollWidth, height: document.documentElement.scrollHeight }));
    await page.screenshot({ path: `${output}/${name}.png`, animations: "disabled", fullPage: true, clip: {
      x: Math.max(0, box.x - 16), y: Math.max(0, box.y - 16),
      width: Math.min(box.width + 360, size.width - box.x + 16),
      height: Math.min(box.height + 32, size.height - box.y + 16),
    } });
  } else {
    const target = selector ? page.locator(selector) : page;
    await target.screenshot({ path: `${output}/${name}.png`, animations: "disabled", ...(selector ? {} : { fullPage: true }) });
  }
  await page.locator("#documentation-annotations").evaluateAll((nodes) => nodes.forEach((node) => node.remove()));
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
  await installer.route(`**/firmware/${version}/initial-flash.bin`, (route) => route.fulfill({ body: image }));
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
  await capture(installer, "install-01-connect", undefined, [
    { selector: "#connect", text: "Start here", x: 610, y: 495, width: 180, edge: "right" },
  ]);
  await installer.getByRole("button", { name: "Connect controller", exact: true }).click();
  await installer.locator("#board-confirm").check();
  await installer.locator("#erase-confirm").check();
  await capture(installer, "install-02-confirm", undefined, [
    { selector: "#version", text: "Choose Recommended", x: 987, y: 814, width: 285, edge: "right" },
    { selector: ".check:has(#board-confirm)", text: "Match your board label", x: 987, y: 880, width: 285, edge: "right" },
    { selector: "#backup", text: "Save optional backup", x: 987, y: 946, width: 285, edge: "right" },
    { selector: ".check:has(#erase-confirm)", text: "Accept settings erase", x: 987, y: 1012, width: 285, edge: "right" },
    { selector: "#install", text: "Click to install", x: 560, y: 1110, width: 235, edge: "right" },
  ]);
  await installer.getByRole("button", { name: "Erase & install firmware", exact: true }).click();
  await installer.getByText("Writing firmware", { exact: true }).waitFor();
  await capture(installer, "install-03-writing", undefined, [
    { selector: "#progress-region", text: "Wait. Keep USB connected.", x: 965, y: 820, width: 350, edge: "right" },
  ]);
  await installer.evaluate(() => window.finishDocumentationFlash());
  await installer.getByRole("heading", { name: "Firmware installed", exact: true }).waitFor();
  await capture(installer, "install-04-key-network", "#setup-options", [
    { selector: "#download-key", text: "Save your private key", x: 750, y: 110, width: 310, edge: "right" },
    { selector: "#network-title", text: "Wait for Ethernet", x: 750, y: 245, width: 310, edge: "right" },
  ]);
  const downloaded = installer.waitForEvent("download");
  await installer.getByRole("button", { name: "Download admin key", exact: true }).click();
  await (await downloaded).delete();
  await capture(installer, "install-07-finish", "#setup-options", [
    { selector: "#finish", text: "Finish after saving key", x: 750, y: 110, width: 310, edge: "right" },
    { selector: "#open-device", text: "Open your controller", x: 750, y: 285, width: 310, edge: "right" },
  ]);
  await installer.getByRole("button", { name: "Finish USB setup", exact: true }).click();
  await installer.getByRole("heading", { name: "USB setup complete", exact: true }).waitFor();
  await capture(installer, "install-05-complete", undefined, [
    { selector: "#open-device", text: "Open device; bookmark it", x: 973, y: 845, width: 335, edge: "right" },
  ]);

  const device = await context.newPage();
  const source = await readFile(new URL("../../main/web/index.html", import.meta.url), "utf8");
  await device.route("**/device", (route) => route.fulfill({ contentType: "text/html", body: source }));
  await device.route("**/api/v1/status", (route) => route.fulfill({ json: status }));
  await device.route("**/api/v1/config", (route) => route.fulfill({ json: config }));
  await device.goto(`${origin}/device`);
  await device.getByText(`192.0.2.140 · firmware ${version} · BACnet/IP online`, { exact: true }).waitFor();
  await device.locator("#keyFile").setInputFiles({ name: "bacnet-io-example-admin.key", mimeType: "text/plain", buffer: Buffer.from(`${exampleKey}\n`) });
  await device.getByText("Admin key loaded locally. The file was not uploaded.", { exact: true }).waitFor();
  await capture(device, "install-06-load-key", undefined, [
    { selector: "#loadKey", text: "Choose the downloaded .key file", x: 740, y: 220, width: 395, edge: "top" },
  ]);
  // Let the transient key-loaded toast disappear before overview screenshots.
  await device.waitForFunction(() => !document.getElementById("toast").classList.contains("show"));
  await capture(device, "firmware-relays");
  await device.getByRole("button", { name: "BACnet & Status", exact: true }).click();
  await capture(device, "firmware-status");
  await device.getByRole("button", { name: "Configuration", exact: true }).click();
  await capture(device, "firmware-configuration");
  await device.getByRole("button", { name: "Firmware", exact: true }).click();
  await capture(device, "firmware-update");
  await capture(device, "update-01-select", undefined, [
    { selector: "#otaFile", text: "Choose firmware-ota.bin", x: 700, y: 235, width: 335, edge: "top" },
  ]);
  const otaExample = Buffer.alloc(512);
  otaExample[0] = 0xe9; otaExample[3] = 0x4f; otaExample[12] = 9;
  otaExample.writeUInt32LE(0xabcd5432, 32);
  otaExample.write(version, 48); otaExample.write(project, 80); otaExample.write("v5.5.4", 144);
  await device.locator("#otaFile").setInputFiles({ name: "firmware-ota.bin", mimeType: "application/octet-stream", buffer: otaExample });
  await device.locator("#otaInfo").filter({ hasText: `${project} ${version}` }).waitFor();
  await capture(device, "update-02-verify", undefined, [
    { selector: "#otaInfo", text: "Check the selected version", x: 745, y: 695, width: 365, edge: "right" },
    { selector: "#uploadOta", text: "Start the Ethernet update", x: 745, y: 570, width: 365, edge: "right" },
  ]);
  await writeFile(`${output}/captures.json`, JSON.stringify({ firmware: version,
    browser: browser.version(), viewport: "1440 x 1080", source: "Actual application interfaces rendered with isolated example data; no live hardware.",
    files: captures }, null, 2) + "\n");
  console.log(`Saved ${captures.length} documentation screenshots to docs/images.`);
} finally {
  await context.close();
  await browser.close();
  await server.close();
}
