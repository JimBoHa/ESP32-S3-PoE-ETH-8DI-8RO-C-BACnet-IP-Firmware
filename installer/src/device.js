// SPDX-License-Identifier: Apache-2.0
import { ESPLoader, Transport } from "esptool-js";
import SparkMD5 from "spark-md5";
import { FLASH_BYTES, validateStatus } from "./firmware.js";
import { SetupClient } from "./usb-setup.js";
import { readFlashBackup, UsbHardReset } from "./flasher.js";
import { readSlipPacket } from "./slip-reader.js";

class BufferedTransport extends Transport {
  read(timeout) { return readSlipPacket(this, timeout); }
  async releaseReader() {
    if (this.reader) await this.reader.cancel();
    await this.waitForUnlock(20);
    this.buffer = new Uint8Array(0);
  }
}

const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export class Controller {
  constructor(port, progress = () => {}) {
    this.port = port;
    this.progress = progress;
    this.setup = null;
    this.loader = null;
    this.transport = null;
    this.identity = null;
  }

  async connect() {
    this.setup = new SetupClient(this.port);
    try {
      await this.setup.open();
      let status;
      for (let attempt = 0; attempt < 3; attempt++) {
        try { status = validateStatus(await this.setup.request("STATUS", 1000)); break; }
        catch (error) { if (attempt === 2) throw error; }
      }
      this.identity = { chip: "ESP32-S3", mac: status.ethernet_mac, running: status };
      return this.identity;
    } catch {
      await this.setup.close().catch(() => {});
      this.setup = null;
      return this.bootloader();
    }
  }

  async bootloader() {
    if (this.loader) return this.identity;
    if (this.setup) await this.setup.close();
    this.setup = null;
    this.transport = new BufferedTransport(this.port, false);
    this.transport.setDeviceLostCallback?.(() => this.progress("disconnected", 0));
    this.loader = new ESPLoader({ transport: this.transport, baudrate: 460800,
      serialOptions: { bufferSize: 65536 },
      resetConstructors: { hardReset: (transport) => new UsbHardReset(transport) },
      terminal: { clean() {}, write() {}, writeLine() {} }, debugLogging: false, enableTracing: false });
    try {
      await this.loader.main();
      if (this.loader.chip.CHIP_NAME !== "ESP32-S3") {
        throw new Error("Wrong chip detected. This installer supports only the Waveshare ESP32-S3-POE-ETH-8DI-8RO-C.");
      }
      const size = await this.loader.detectFlashSize();
      if (size !== "16MB") throw new Error(`Wrong flash size detected (${size}). This board requires 16 MB.`);
      const mac = await this.loader.chip.readMac(this.loader);
      this.identity = { chip: "ESP32-S3", flashBytes: FLASH_BYTES, mac };
      return this.identity;
    } catch (error) {
      await this.disconnect().catch(() => {});
      throw error;
    }
  }

  async backup() {
    await this.bootloader();
    this.progress("backup", 0);
    const mac = this.identity.mac;
    try {
      return await readFlashBackup(this.loader, FLASH_BYTES,
        (amount) => this.progress("backup", amount), async () => {
          // Do not boot the application between backup chunks: it changes
          // the reboot counter in NVS and would invalidate the snapshot.
          await this.transport.disconnect().catch(() => {});
          this.loader = null; this.transport = null;
          await this.bootloader();
          if (this.identity.mac !== mac) throw new Error("Controller changed during backup. Reconnect and start again.");
          return this.loader;
        });
    } catch (error) {
      await this.disconnect().catch(() => {});
      const reason = /^(Backup |Serial transfer|Invalid serial|Controller )/.test(error.message) ? ` ${error.message}` : "";
      throw new Error(`Recovery backup failed.${reason} Nothing was erased. Disconnect and reconnect here to retry.`);
    }
  }

  async install(bytes) {
    await this.bootloader();
    this.progress("erase", 0);
    await this.loader.writeFlash({
      fileArray: [{ data: bytes, address: 0 }], flashMode: "keep", flashFreq: "keep",
      flashSize: "keep", eraseAll: true, compress: true,
      reportProgress: (_file, written, total) => this.progress("write", written / total),
      calculateMD5Hash: (image) => new SparkMD5.ArrayBuffer().append(image).end(),
    });
    this.progress("verified", 1);
  }

  async startApplication(expectedVersion) {
    if (this.loader) await this.loader.after("hard_reset");
    // Keep the OS serial port open: close/reopen can reset native USB again,
    // ending the newly generated key's first-boot export window.
    if (this.transport) await this.transport.releaseReader();
    this.loader = null;
    this.transport = null;
    await pause(500);
    this.setup = new SetupClient(this.port);
    await this.setup.open();
    let lastError;
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
      try {
        const status = validateStatus(await this.setup.request("STATUS", 1200), expectedVersion);
        this.identity = { chip: "ESP32-S3", mac: status.ethernet_mac, running: status };
        return status;
      } catch (error) { lastError = error; }
      await pause(250);
    }
    throw lastError;
  }

  async status() {
    if (!this.setup) return this.startApplication();
    return validateStatus(await this.setup.request("STATUS"));
  }

  async key() {
    if (!this.setup) await this.startApplication();
    const response = await this.setup.request("KEY");
    if (!/^[a-f0-9]{64}$/.test(response.admin_key)) throw new Error("Controller returned an invalid key. Please retry.");
    return response.admin_key;
  }

  async lock() {
    if (this.setup) await this.setup.request("LOCK");
  }

  async disconnect() {
    if (this.setup) await this.setup.close();
    this.setup = null;
    if (this.loader) await this.loader.after("hard_reset").catch(() => {});
    if (this.transport) await this.transport.disconnect();
    this.loader = null;
    this.transport = null;
  }
}
