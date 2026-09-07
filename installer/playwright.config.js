import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./tests/browser",
  timeout: 20000,
  expect: { timeout: 5000 },
  use: { baseURL: "http://127.0.0.1:5174", headless: true, screenshot: "only-on-failure" },
  webServer: { command: "npm run dev -- --port 5174", url: "http://127.0.0.1:5174", reuseExistingServer: !process.env.CI },
});
