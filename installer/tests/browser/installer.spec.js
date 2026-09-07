import { test, expect } from "@playwright/test";
import { createHash } from "node:crypto";

const image = Buffer.alloc(300);
image[0] = 0xe9; image[3] = 0x4f; image[12] = 9;
const asset = { path: "0.14.0/initial-flash.bin", bytes: image.length, sha256: createHash("sha256").update(image).digest("hex") };
const catalog = { schema: 1, recommended: "0.14.0", releases: [{ version: "0.14.0", project: "esp32_s3_poe_eth_8di_8ro_bacnet", chipFamily: "ESP32-S3", flashBytes: 16777216, usbSetupProtocol: 1, initial: asset, ota: { ...asset, path: "0.14.0/firmware-ota.bin" } }] };
const fakeKey = "ab".repeat(32);

// Intercept only the device adapter in the development test server. Production
// code has no test flags, simulated devices, injected drivers, or test keys.
async function fixture(page, scenario = {}) {
  await page.addInitScript((options) => {
    window.scenario = options;
    window.operations = [];
    Object.defineProperty(navigator, "serial", { configurable: true, value: { requestPort: async () => ({}) } });
  }, scenario);
  await page.route("**/firmware/catalog.json", (route) => route.fulfill({ json: catalog }));
  await page.route("**/firmware/0.14.0/initial-flash.bin", (route) => route.fulfill({ body: scenario.corrupt ? Buffer.alloc(300) : image }));
  await page.route("**/src/device.js", (route) => route.fulfill({ contentType: "application/javascript", body: `
    const status = () => ({ ok:true, protocol:1, project:"esp32_s3_poe_eth_8di_8ro_bacnet", firmware_version:"0.14.0", ethernet_mac:"02:00:00:12:34:56", ip_address:window.scenario.noNetwork?"":"192.0.2.140", hostname:"bacnet-io-123456", device_instance:123456, key_export_allowed:!window.scenario.locked, ethernet_link:!window.scenario.noNetwork, relay_controller_healthy:!window.scenario.hardwareFault, rtc_present:true });
    export class Controller {
      constructor(port, progress) { this.progress=progress; this.identity={chip:"ESP32-S3",mac:"02:00:00:12:34:50"}; }
      async connect() { if(window.scenario.wrongChip) throw new Error("Wrong chip detected. This installer supports only the Waveshare ESP32-S3-POE-ETH-8DI-8RO-C."); if(window.scenario.existing){this.setup=true;return {...this.identity,running:status()};} return this.identity; }
      async backup() { window.operations.push("backup");this.progress("backup",1);return new Uint8Array(16777216); }
      async install() { window.operations.push("erase");this.progress("erase",0);if(window.scenario.flashFailure)throw new Error("USB disconnected during flash. Reconnect and install again.");window.operations.push("write");this.progress("verified",1); }
      async startApplication() { this.setup=true;if(window.scenario.bootFailure)throw new Error("timeout");return status(); }
      async status() { if(window.scenario.bootFailure)throw new Error("timeout");return status(); }
      async key() { if(window.scenario.locked)throw new Error("Hold BOOT for 3 seconds while the device is running, release it, then download the key again.");return "${fakeKey}"; }
      async lock() { window.operations.push("lock"); }
      async disconnect() { this.setup=false;window.operations.push("disconnect"); }
    }
  ` }));
  await page.goto("/");
}

async function connectAndConfirm(page) {
  await page.getByRole("button", { name: "Connect controller" }).click();
  await expect(page.getByText("USB connected", { exact: true })).toBeVisible();
  await page.locator("#board-confirm").check();
  await page.locator("#erase-confirm").check();
}

test("complete install, private key download, Ethernet handoff, and lock", async ({ page }) => {
  await fixture(page);
  await connectAndConfirm(page);
  await page.getByRole("button", { name: "Erase & install firmware" }).click();
  await expect(page.getByRole("heading", { name: "Firmware installed", exact: true })).toBeVisible();
  await expect(page.getByRole("link", { name: "Open device", exact: true })).toHaveAttribute("href", "http://192.0.2.140/");
  const downloaded = page.waitForEvent("download");
  await page.getByRole("button", { name: "Download admin key", exact: true }).click();
  const download = await downloaded;
  expect(download.suggestedFilename()).toBe("bacnet-io-020000123456-admin.key");
  const stream = await download.createReadStream();
  let content = ""; for await (const part of stream) content += part;
  expect(content).toBe(fakeKey + "\n");
  expect(await page.locator("body").innerText()).not.toContain(fakeKey);
  expect(await page.evaluate(() => [localStorage.length, sessionStorage.length])).toEqual([0, 0]);
  await page.getByRole("button", { name: "Finish USB setup" }).click();
  await expect(page.getByRole("heading", { name: "USB setup complete" })).toBeVisible();
  expect(await page.evaluate(() => window.operations)).toEqual(["erase", "write", "lock", "disconnect"]);
});

test("board and erase confirmations gate installation independently", async ({ page }) => {
  await fixture(page);
  await page.getByRole("button", { name: "Connect controller" }).click();
  const install = page.getByRole("button", { name: "Erase & install firmware" });
  await expect(install).toBeDisabled();
  await page.locator("#erase-confirm").check();
  await expect(install).toBeDisabled();
  await page.locator("#board-confirm").check();
  await expect(install).toBeEnabled();
  await page.locator("#erase-confirm").uncheck();
  await expect(install).toBeDisabled();
});

test("corrupt firmware never reaches erase", async ({ page }) => {
  await fixture(page, { corrupt: true }); await connectAndConfirm(page);
  await page.getByRole("button", { name: "Erase & install firmware" }).click();
  await expect(page.getByRole("status")).toContainText("checksum failed");
  expect(await page.evaluate(() => window.operations)).toEqual([]);
});

test("wrong chip cannot enter installation", async ({ page }) => {
  await fixture(page, { wrongChip: true });
  await page.getByRole("button", { name: "Connect controller" }).click();
  await expect(page.getByRole("status")).toContainText("Wrong chip detected");
  await expect(page.locator("#install-options")).toBeHidden();
});

test("interrupted setup preserves successful flash result and directs reconnection", async ({ page }) => {
  await page.clock.install();
  await fixture(page, { bootFailure: true }); await connectAndConfirm(page);
  await page.getByRole("button", { name: "Erase & install firmware" }).click();
  await expect(page.getByRole("status")).toContainText("written and verified");
  await page.clock.runFor(3000);
  await expect(page.getByRole("status")).toContainText("written and verified");
  await expect(page.getByRole("status")).toContainText("do not need to erase again");
  await expect(page.getByRole("button", { name: "Disconnect", exact: true })).toBeEnabled();
});

test("mid-flash disconnect shows recovery and never claims installed", async ({ page }) => {
  await fixture(page, { flashFailure: true }); await connectAndConfirm(page);
  await page.getByRole("button", { name: "Erase & install firmware" }).click();
  await expect(page.getByRole("status")).toContainText("disconnected during flash");
  await expect(page.getByRole("heading", { name: "Firmware installed", exact: true })).toBeHidden();
});

test("existing firmware supports locked-key recovery without reinstallation", async ({ page }) => {
  await fixture(page, { existing: true, locked: true });
  await page.getByRole("button", { name: "Connect controller" }).click();
  await expect(page.locator("#unlock-help")).toContainText("BOOT for 3 seconds");
  await page.getByRole("button", { name: "Download admin key", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("Hold BOOT for 3 seconds");
  expect(await page.evaluate(() => window.operations)).toEqual([]);
});

test("no Ethernet is an actionable waiting state, not an installation failure", async ({ page }) => {
  await fixture(page, { noNetwork: true }); await connectAndConfirm(page);
  await page.getByRole("button", { name: "Erase & install firmware" }).click();
  await expect(page.getByRole("heading", { name: "Firmware installed", exact: true })).toBeVisible();
  await expect(page.getByRole("heading", { name: "Connect Ethernet", exact: true })).toBeVisible();
  await expect(page.getByRole("link", { name: "Open device", exact: true })).toBeHidden();
});

test("successful flashing does not hide a hardware health fault", async ({ page }) => {
  await fixture(page, { hardwareFault: true }); await connectAndConfirm(page);
  await page.getByRole("button", { name: "Erase & install firmware" }).click();
  await expect(page.getByRole("heading", { name: "Firmware installed", exact: true })).toBeVisible();
  await expect(page.locator("#health-warning")).toBeVisible();
  await expect(page.locator("#health-relays")).toHaveText("Fault reported");
});

test("backup downloads full flash before any erase", async ({ page }) => {
  await fixture(page); await connectAndConfirm(page);
  const downloaded = page.waitForEvent("download");
  await page.getByRole("button", { name: "Download backup", exact: true }).click();
  const download = await downloaded;
  expect(download.suggestedFilename()).toMatch(/full-backup.*\.bin$/);
  const stream = await download.createReadStream();
  let bytes = 0; for await (const part of stream) bytes += part.length;
  expect(bytes).toBe(16777216);
  expect(await page.evaluate(() => window.operations)).toEqual(["backup"]);
});

test("unsupported browser has clear instructions and disables USB connect", async ({ page }) => {
  await fixture(page);
  await page.addInitScript(() => Object.defineProperty(navigator, "serial", { configurable: true, value: undefined }));
  await page.reload();
  await expect(page.locator("#unsupported")).toBeVisible();
  await expect(page.getByRole("button", { name: "Connect controller" })).toBeDisabled();
});

test("mobile layout and enlarged text retain usable controls without horizontal overflow", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await fixture(page, { existing: true });
  await page.getByRole("button", { name: "Connect controller" }).click();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: "test-results/installer-mobile.png", fullPage: true });
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.evaluate(() => { document.documentElement.style.fontSize = "32px"; });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});
