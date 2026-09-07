import { test, expect } from "@playwright/test";
import { readFile } from "node:fs/promises";

const pageSource = await readFile(new URL("../../../main/web/index.html", import.meta.url), "utf8");
const key = "cd".repeat(32);

async function devicePage(page) {
  const requests = [];
  await page.route("**/api/v1/**", (route) => {
    requests.push({ method: route.request().method(), body: route.request().postData() });
    return route.fulfill({ json: {} });
  });
  await page.route("**/device-test", (route) => route.fulfill({ contentType: "text/html", body: pageSource }));
  await page.goto("/device-test");
  return requests;
}

test("device management loads downloaded key locally and keeps it masked", async ({ page }) => {
  const requests = await devicePage(page);
  await page.locator("#keyFile").setInputFiles({ name: "device-admin.key", mimeType: "text/plain", buffer: Buffer.from(`${key}\n`) });
  await expect(page.locator("#adminKey")).toHaveValue(key);
  await expect(page.locator("#adminKey")).toHaveAttribute("type", "password");
  await expect(page.getByText("Admin key loaded locally. The file was not uploaded.")).toBeVisible();
  expect(requests.every((request) => request.method === "GET" && !request.body)).toBe(true);
  expect(await page.evaluate(() => [localStorage.length, sessionStorage.length])).toEqual([0, 0]);
  await page.locator("#clearKey").click();
  await expect(page.locator("#adminKey")).toHaveValue("");
});

test("device management rejects invalid and oversized key files", async ({ page }) => {
  await devicePage(page);
  await page.locator("#keyFile").setInputFiles({ name: "bad.key", mimeType: "text/plain", buffer: Buffer.from("invalid") });
  await expect(page.getByText("The key file must contain exactly 64 hexadecimal characters.")).toBeVisible();
  await expect(page.locator("#adminKey")).toHaveValue("");
  await page.locator("#keyFile").setInputFiles({ name: "large.key", mimeType: "text/plain", buffer: Buffer.alloc(257) });
  await expect(page.getByText("Select the small admin key file downloaded during USB setup.")).toBeVisible();
  await expect(page.locator("#adminKey")).toHaveValue("");
});
