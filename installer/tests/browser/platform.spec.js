import { test, expect } from "@playwright/test";

test("browser exposes the native APIs required for USB installation", async ({ page, browser }, testInfo) => {
  // Do not substitute navigator.serial here. This verifies browser capabilities;
  // the physical USB chooser and OS driver still require a connected controller.
  await page.goto("/");
  const capabilities = await page.evaluate(() => ({
    secureContext: window.isSecureContext,
    serial: typeof navigator.serial?.requestPort === "function",
    serialSignals: typeof globalThis.SerialPort?.prototype.setSignals === "function",
    digest: typeof crypto.subtle?.digest === "function",
    streams: typeof ReadableStream === "function" && typeof WritableStream === "function",
    fileRead: typeof File.prototype.arrayBuffer === "function",
    downloads: "download" in document.createElement("a"),
  }));
  expect(Object.values(capabilities).every(Boolean)).toBe(true);
  await expect(page.getByRole("button", { name: "Connect controller", exact: true })).toBeEnabled();
  console.log(JSON.stringify({ platform: process.platform, architecture: process.arch,
    browser: testInfo.project.name, version: browser.version(), capabilities }));
});
