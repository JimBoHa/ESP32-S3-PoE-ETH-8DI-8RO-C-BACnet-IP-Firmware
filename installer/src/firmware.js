// SPDX-License-Identifier: 0BSD
export const PROJECT = "esp32_s3_poe_eth_8di_8ro_bacnet";
export const FLASH_BYTES = 16 * 1024 * 1024;
export const MODEL = "ESP32-S3-POE-ETH-8DI-8RO-C";
const VERSION = /^\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?$/;
const HASH = /^[a-f0-9]{64}$/;

export function assetURL(path, base) {
  if (typeof path !== "string" || !/^[a-zA-Z0-9._/-]+$/.test(path) ||
      path.split("/").some((part) => part === ".." || part === "")) {
    throw new Error("Firmware package contains an unsafe file path.");
  }
  const url = new URL(path, base);
  const directory = new URL(".", base);
  if (url.origin !== directory.origin || !url.pathname.startsWith(directory.pathname)) {
    throw new Error("Firmware files must be hosted with this installer.");
  }
  return url;
}

export function validateCatalog(value, base) {
  if (value?.schema !== 1 || !Array.isArray(value.releases) || !value.releases.length ||
      value.releases.length > 50 || !VERSION.test(value.recommended)) {
    throw new Error("No approved firmware package is available. Please contact the project maintainer.");
  }
  const seen = new Set();
  for (const release of value.releases) {
    if (!VERSION.test(release.version) || seen.has(release.version) ||
        release.project !== PROJECT || release.chipFamily !== "ESP32-S3" ||
        release.flashBytes !== FLASH_BYTES || release.usbSetupProtocol !== 1) {
      throw new Error("Firmware package does not match this controller.");
    }
    seen.add(release.version);
    for (const name of ["initial", "ota"]) {
      const asset = release[name];
      if (!asset || !HASH.test(asset.sha256) || !Number.isInteger(asset.bytes) ||
          asset.bytes < 256 || asset.bytes > FLASH_BYTES) {
        throw new Error("Firmware package is missing its integrity information.");
      }
      assetURL(asset.path, base);
    }
  }
  if (!seen.has(value.recommended)) throw new Error("Recommended firmware is missing.");
  return value;
}

export async function sha256(bytes) {
  const hash = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(hash)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

export async function verifyImage(bytes, asset) {
  if (bytes.byteLength !== asset.bytes || await sha256(bytes) !== asset.sha256) {
    throw new Error("Firmware checksum failed. Nothing has been erased. Reload and try again.");
  }
  // ESP image header: magic, chip ID (ESP32-S3 = 9), and 16 MB flash encoding.
  if (bytes[0] !== 0xe9 || bytes[12] !== 9 || bytes[13] !== 0 || bytes[3] >> 4 !== 4) {
    throw new Error("Firmware image is not for the supported ESP32-S3 / 16 MB board.");
  }
  return bytes;
}

export function deviceURL(address) {
  if (typeof address !== "string" || !/^\d{1,3}(\.\d{1,3}){3}$/.test(address)) return null;
  const parts = address.split(".").map(Number);
  if (parts.some((part) => part > 255) || parts[0] === 0 || parts[0] >= 224) return null;
  return `http://${parts.join(".")}/`;
}

export function validateStatus(status, expectedVersion) {
  if (!status?.ok || status.protocol !== 1 || status.project !== PROJECT ||
      !VERSION.test(status.firmware_version) ||
      (expectedVersion && status.firmware_version !== expectedVersion)) {
    throw new Error("The expected firmware has not started yet.");
  }
  if (typeof status.ethernet_mac !== "string" || !/^([a-f0-9]{2}:){5}[a-f0-9]{2}$/i.test(status.ethernet_mac)) {
    throw new Error("Controller returned an invalid device identity.");
  }
  return status;
}
