import { defineConfig } from "@playwright/test";

const channels = process.env.INSTALLER_BROWSERS?.split(",") || ["chromium"];

export default defineConfig({
  testDir: "./tests/browser",
  timeout: 20000,
  expect: { timeout: 5000 },
  use: { baseURL: "http://127.0.0.1:5174", headless: true, screenshot: "only-on-failure" },
  projects: channels.map((channel) => ({ name: channel,
    use: channel === "chromium" ? {} : { channel } })),
  webServer: { command: "npm run dev -- --port 5174", url: "http://127.0.0.1:5174", reuseExistingServer: !process.env.CI },
});
