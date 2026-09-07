// SPDX-License-Identifier: 0BSD
import SparkMD5 from "spark-md5";

const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const words = (...values) => {
  const bytes = new Uint8Array(values.length * 4);
  const view = new DataView(bytes.buffer);
  values.forEach((value, index) => view.setUint32(index * 4, value, true));
  return bytes;
};

// esptool-js 0.6.1's HardReset only deasserts RTS. Native USB Serial/JTAG
// needs an asserted reset pulse, matching Espressif's Python reset strategy.
export class UsbHardReset {
  constructor(transport) { this.transport = transport; }
  async reset() {
    await this.transport.device.setSignals({ dataTerminalReady: false, requestToSend: true });
    await pause(100);
    await this.transport.device.setSignals({ dataTerminalReady: false, requestToSend: false });
    await pause(200);
  }
}

// esptool-js 0.6.1 readFlash requests 1024 in-flight blocks and leaves the
// stub's final digest unread. A single-block window limits native USB
// overruns; preallocation avoids repeatedly copying a growing 16 MB array.
// Protocol: https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/serial-protocol.html
export async function readFlashRegion(loader, offset, size, onProgress = () => {}) {
  const blockSize = 4096;
  const result = await loader.checkCommand("read flash", loader.ESP_READ_FLASH,
    words(offset, size, blockSize, 1));
  if (result !== 0) throw new Error("Controller could not start the recovery backup. Nothing was erased.");
  const bytes = new Uint8Array(size);
  let received = 0;
  while (received < size) {
    const packet = await loader.transport.read(10000);
    if (packet.length !== Math.min(blockSize, size - received)) {
      throw new Error(`Backup received an incomplete USB packet (${packet.length} of ${Math.min(blockSize, size - received)} bytes).`);
    }
    bytes.set(packet, received);
    received += packet.length;
    await loader.transport.write(words(received));
    onProgress(received / size);
  }
  const digest = await loader.transport.read(10000);
  const expected = [...digest].map((byte) => byte.toString(16).padStart(2, "0")).join("");
  if (digest.length !== 16 || SparkMD5.ArrayBuffer.hash(bytes) !== expected) {
    throw new Error("Backup checksum failed. Nothing was erased. Disconnect and reconnect to retry.");
  }
  return bytes;
}

export async function readFlashBackup(loader, size, onProgress, reconnect) {
  const bytes = new Uint8Array(size);
  const chunkSize = 256 * 1024;
  for (let offset = 0; offset < size; offset += chunkSize) {
    const length = Math.min(chunkSize, size - offset);
    for (let attempt = 0; attempt < 3; attempt++) {
      try {
        const chunk = await readFlashRegion(loader, offset, length,
          (amount) => onProgress((offset + amount * length) / size));
        bytes.set(chunk, offset);
        break;
      } catch (error) {
        if (attempt === 2) throw error;
        // Reset directly into the ROM downloader, without booting the app
        // and changing NVS mid-backup. The caller rechecks device identity.
        loader = await reconnect();
        onProgress(offset / size);
      }
    }
  }
  // Also prove all individually verified chunks form one current snapshot.
  if (await loader.flashMd5sum(0, size) !== SparkMD5.ArrayBuffer.hash(bytes)) {
    throw new Error("Backup changed during transfer. Please reconnect and retry.");
  }
  return bytes;
}
