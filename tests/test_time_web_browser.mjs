/* SPDX-License-Identifier: 0BSD */
// Actual embedded page in Chromium; all requests intercepted in-memory.
// Requires the pinned installer dev dependencies and Playwright Chromium.
import assert from 'node:assert/strict';
import { createHash, createHmac } from 'node:crypto';
import { readFile, mkdtemp } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { chromium, expect } from '../installer/node_modules/@playwright/test/index.mjs';

const html = await readFile(new URL('../main/web/index.html', import.meta.url), 'utf8');
const dummyKey = '42'.repeat(32); // Public test fixture, never a device credential.
const nonce = 'a1'.repeat(16);
const pacific = 'PST8PDT,M3.2.0/2,M11.1.0/2';
const config = {
  device_instance: 600001, device_instance_auto: false, device_instance_locked: true,
  bacnet_port: 47808, vendor_id: 0, input_invert_mask: 0, dhcp_enabled: true,
  restore_relay_state: false, hostname: 'mock-controller', device_name: 'Mock controller',
  vendor_name: 'Test fixture', location: 'Isolated browser test', database_revision: 7,
  ip_address: '192.0.2.10', netmask: '255.255.255.0', gateway: '192.0.2.1', dns_server: '192.0.2.1',
  input_names: Array.from({length: 8}, (_, i) => `Input ${i + 1}`),
  relay_names: Array.from({length: 8}, (_, i) => `Output ${i + 1}`),
};
let savedTime = {ntp_server: 'pool.ntp.org', timezone: 'UTC0'};
let statusReads = 0;
let timeWrites = 0;
let rejectNextSave = false;
let holdNextSave = false;
let releaseSave;
const failures = [];
const pageErrors = [];
const status = () => ({
  product: 'Isolated mock controller', firmware_version: 'test', build_date: 'test', running_partition: 'mock',
  uptime_seconds: 30, reboot_count: 1, last_reset_reason: 'software', last_reset_reason_code: 3,
  ip_address: '192.0.2.10', ethernet_link: true, ipv4_assigned: true, bacnet_running: true,
  bacnet_device_instance: 600001, bacnet_instance_status: 'manual', bacnet_udp_port: 47808,
  bacnet_udp_receive_mailbox_size: 64, bacnet_packets_received: statusReads,
  digital_inputs_mask: 0, relay_outputs_mask: 0, relay_commands_mask: 0,
  relay_active_priorities: Array(8).fill(0), relay_controller_healthy: true, rtc_present: true,
  free_heap_bytes: 260000, minimum_free_heap_bytes: 250000,
  bacnet_restart: {timestamp_frozen: true, timestamp_local: '2026-09-11T06:00:00.00', timestamp_clock: 'ntp', notifications_sent: 1, pending_recipients: 0},
  bacnet_announcement: {transport_acceptances: 1, attempts: 1},
  time: {...savedTime, valid: true, synchronized: true, sntp_running: true, source: 'ntp',
    utc_time: '2026-09-11T13:00:00Z', local_time: savedTime.timezone === pacific ? '2026-09-11T06:00:00-0700' : '2026-09-11T13:00:00+0000',
    last_sync_unix_us: 1789131600000000, sync_age_seconds: 1},
});

const browser = await chromium.launch({headless: true});
try {
  const page = await browser.newPage({viewport: {width: 1280, height: 900}});
  page.setDefaultTimeout(10000);
  page.on('pageerror', error => pageErrors.push(error.message));
  await page.route('**/*', async route => {
    try {
      const request = route.request();
      const url = new URL(request.url());
      assert.equal(url.origin, 'http://127.0.0.1:18765', 'No external or real-device request is allowed');
      const json = value => route.fulfill({status: 200, contentType: 'application/json', body: JSON.stringify(value)});
      if (url.pathname === '/') return route.fulfill({status: 200, contentType: 'text/html', body: html});
      if (url.pathname === '/favicon.ico') return route.fulfill({status: 204, body: ''});
      if (request.method() === 'GET') {
        if (url.pathname === '/api/v1/status') { statusReads++; return json(status()); }
        if (url.pathname === '/api/v1/config') return json(config);
        if (url.pathname === '/api/v1/time') return json({...savedTime, time: status().time});
        if (url.pathname === '/api/v1/auth/challenge') return json({nonce});
      }
      assert.equal(request.method(), 'PUT');
      assert.equal(url.pathname, '/api/v1/time', 'Only time configuration may be mutated');
      const body = request.postDataBuffer();
      const hash = createHash('sha256').update(body).digest('hex');
      const canonical = `BACNET-IO-AUTH-V1\nPUT\n/api/v1/time\n${nonce}\n${body.length}\n${hash}\n`;
      const signature = createHmac('sha256', Buffer.from(dummyKey, 'hex')).update(canonical).digest('hex');
      const headers = request.headers();
      assert.equal(headers['x-auth-nonce'], nonce);
      assert.equal(headers['x-content-sha256'], hash);
      assert.equal(headers['x-authorization'], signature);
      timeWrites++;
      if (rejectNextSave) {
        rejectNextSave = false;
        return route.fulfill({status: 400, contentType: 'application/json', body: JSON.stringify({error: 'timezone must be a valid POSIX rule'})});
      }
      if (holdNextSave) {
        holdNextSave = false;
        await new Promise(resolve => { releaseSave = resolve; });
      }
      savedTime = JSON.parse(body.toString('utf8'));
      assert.deepEqual(Object.keys(savedTime).sort(), ['ntp_server', 'timezone']);
      return json({saved: true, persistent: true, reboot_required: false, apply_pending: true});
    } catch (error) {
      failures.push(error);
      await route.abort();
    }
  });
  await page.goto('http://127.0.0.1:18765/');
  await page.locator('[data-panel="config"]').click();
  await expect(page.locator('#tNtpServer')).toHaveValue('pool.ntp.org');
  await expect(page.locator('#tTimezonePreset')).toHaveValue('UTC0');
  await expect(page.locator('#tTimezoneCustom')).toBeHidden();
  await expect(page.locator('#saveTime')).toBeDisabled();
  await page.locator('[data-panel="io"]').click();
  await page.locator('#keyFile').setInputFiles({name: 'public-dummy-test.key', mimeType: 'text/plain', buffer: Buffer.from(dummyKey)});
  await page.locator('[data-panel="config"]').click();
  await expect(page.locator('#saveTime')).toBeEnabled();

  await page.locator('#tTimezonePreset').selectOption(pacific);
  await page.locator('#tNtpServer').fill('time.example.test');
  await page.locator('#saveTime').click();
  await expect(page.locator('#toast')).toContainText('Time settings saved');
  assert.deepEqual(savedTime, {ntp_server: 'time.example.test', timezone: pacific});
  await expect(page.locator('#tTimezonePreset')).toHaveValue(pacific);
  await expect(page.locator('#tTimezoneCustom')).toBeHidden();

  await page.locator('#tTimezonePreset').selectOption('custom');
  await expect(page.locator('#tTimezoneCustom')).toBeVisible();
  await page.locator('#tTimezoneCustom').fill('IST-5:30');
  await page.locator('#tNtpServer').fill('unsaved.example.test');
  const beforeRefresh = statusReads;
  await expect.poll(() => statusReads).toBeGreaterThan(beforeRefresh);
  await expect(page.locator('#tNtpServer')).toHaveValue('unsaved.example.test');
  await expect(page.locator('#tTimezoneCustom')).toHaveValue('IST-5:30');
  await expect(page.locator('#tTimezonePreset')).toHaveValue('custom');

  rejectNextSave = true;
  await page.locator('#saveTime').click();
  await expect(page.locator('#toast')).toContainText('timezone must be a valid POSIX rule');
  await expect(page.locator('#saveTime')).toBeEnabled();
  await expect(page.locator('#tNtpServer')).toHaveValue('unsaved.example.test');
  assert.equal(savedTime.ntp_server, 'time.example.test');

  await page.locator('#saveTime').click();
  await expect(page.locator('#toast')).toContainText('Time settings saved');
  assert.deepEqual(savedTime, {ntp_server: 'unsaved.example.test', timezone: 'IST-5:30'});
  await expect(page.locator('#tTimezonePreset')).toHaveValue('IST-5:30');

  // Edits made while the asynchronous save is in flight must not disappear.
  holdNextSave = true;
  await page.locator('#tNtpServer').fill('submitted.example.test');
  await page.locator('#saveTime').click();
  await expect.poll(() => typeof releaseSave).toBe('function');
  await page.locator('#tNtpServer').fill('new-unsaved.example.test');
  releaseSave();
  await expect(page.locator('#toast')).toContainText('Time settings saved');
  await expect(page.locator('#tNtpServer')).toHaveValue('new-unsaved.example.test');
  assert.equal(savedTime.ntp_server, 'submitted.example.test');
  await page.locator('#reloadTime').click();
  await expect(page.locator('#tNtpServer')).toHaveValue('submitted.example.test');
  await page.locator('#tTimezonePreset').selectOption(pacific);
  await page.locator('#tNtpServer').fill('pool.ntp.org');
  await page.locator('#saveTime').click();
  await expect(page.locator('#toast')).toContainText('Time settings saved');
  await page.locator('[data-panel="system"]').click();
  await expect(page.locator('#sClockState')).toContainText('NTP synchronized');
  await expect(page.locator('#sLocalTime')).toContainText('06:00:00-0700');
  await expect(page.locator('#sRestartClock')).toHaveText('ntp');
  await expect(page.locator('#sAnnouncements')).toHaveText('1 / 1');
  await page.locator('#toast').evaluate(node => { node.className = ''; });
  const temporary = await mkdtemp(join(tmpdir(), 'time-web-browser-'));
  await page.screenshot({path: join(temporary, 'time-status.png'), fullPage: true});
  await page.locator('[data-panel="config"]').click();
  await page.locator('#timeForm').scrollIntoViewIfNeeded();
  await page.locator('#timeForm').screenshot({path: join(temporary, 'time-form.png')});
  await page.setViewportSize({width: 390, height: 844});
  await page.locator('#timeForm').scrollIntoViewIfNeeded();
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
  await page.locator('#timeForm').screenshot({path: join(temporary, 'time-form-mobile.png')});
  assert.equal(timeWrites, 5);
  assert.deepEqual(pageErrors, []);
  assert.deepEqual(failures, []);
  console.log(`Embedded time UI Chromium tests passed: HMAC, presets, custom TZ, dirty refresh, save failure, in-flight edits, status. Screenshots: ${temporary}`);
} finally {
  releaseSave?.();
  await browser.close();
}
