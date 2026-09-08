import test from "node:test";
import assert from "node:assert/strict";
import { assetURL, deviceURL, validateCatalog, verifyImage, sha256, validateStatus, PROJECT, FLASH_BYTES } from "../src/firmware.js";

const base = new URL("https://example.test/controller/firmware/catalog.json");
const image = new Uint8Array(300);
image[0] = 0xe9; image[3] = 0x4f; image[12] = 9;
const asset = { path: "0.14.0/initial-flash.bin", bytes: image.length, sha256: await sha256(image) };
const release = { version: "0.14.0", project: PROJECT, chipFamily: "ESP32-S3", flashBytes: FLASH_BYTES,
  usbSetupProtocol: 1, initial: asset, ota: { ...asset, path: "0.14.0/firmware-ota.bin" } };
const catalog = () => ({ schema: 1, recommended: "0.14.0", releases: [structuredClone(release)] });

test("catalog resolves firmware inside a GitHub Pages repository subpath", () => {
  assert.equal(validateCatalog(catalog(), base).recommended, "0.14.0");
  assert.equal(assetURL(asset.path, base).href, "https://example.test/controller/firmware/0.14.0/initial-flash.bin");
});

test("catalog rejects path escape, foreign projects, missing checksums, duplicates, and missing recommendation", () => {
  for (const path of ["../other.bin", "/absolute.bin", "https://other.test/image.bin", "a//b.bin", "a/%2e%2e/b.bin"]) {
    assert.throws(() => assetURL(path, base));
  }
  for (const mutate of [
    (c) => { c.releases[0].project = "other"; },
    (c) => { c.releases[0].initial.sha256 = ""; },
    (c) => { c.releases[0].chipFamily = "ESP32"; },
    (c) => { c.releases[0].flashBytes = 4 * 1024 * 1024; },
    (c) => { c.releases[0].usbSetupProtocol = 0; },
    (c) => { c.releases.push(c.releases[0]); },
    (c) => { c.recommended = "99.0.0"; },
  ]) {
    const value = catalog(); mutate(value); assert.throws(() => validateCatalog(value, base));
  }
});

test("firmware validation rejects corrupted, truncated, and wrong-chip images before erase", async () => {
  assert.equal(await verifyImage(image, asset), image);
  const corrupt = image.slice(); corrupt[255] ^= 1;
  await assert.rejects(verifyImage(corrupt, asset), /checksum/);
  await assert.rejects(verifyImage(image.slice(0, -1), asset), /checksum/);
  const wrong = image.slice(); wrong[12] = 0;
  await assert.rejects(verifyImage(wrong, { ...asset, sha256: await sha256(wrong) }), /not for/);
});

test("device links accept IPv4 only and cannot carry script, credentials, or URL parameters", () => {
  assert.equal(deviceURL("192.0.2.140"), "http://192.0.2.140/");
  for (const value of ["0.0.0.0", "256.1.2.3", "224.1.2.3", "javascript:alert(1)", "10.0.0.1/?key=x", "user@10.0.0.1", null]) {
    assert.equal(deviceURL(value), null);
  }
});

test("boot verification requires the expected project, version, and valid MAC", () => {
  const status = { ok: true, protocol: 1, project: PROJECT, firmware_version: "0.14.0", ethernet_mac: "02:00:00:12:34:56" };
  assert.equal(validateStatus(status, "0.14.0"), status);
  assert.throws(() => validateStatus(status, "0.13.5"));
  assert.throws(() => validateStatus({ ...status, project: "other" }));
  assert.throws(() => validateStatus({ ...status, ethernet_mac: "invalid" }));
});
