import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFlashBackup, readFlashRegion, UsbHardReset } from "../src/flasher.js";

function stub(bytes, packets) {
  const acknowledgements = [];
  const frames = packets || [bytes.slice(0, 4096), bytes.slice(4096),
    new Uint8Array(createHash("md5").update(bytes).digest())];
  return {
    ESP_READ_FLASH: 0xd2, acknowledgements, frames,
    async checkCommand(_name, command, payload) {
      assert.equal(command, 0xd2);
      const view = new DataView(payload.buffer);
      assert.deepEqual([0, 4, 8, 12].map((i) => view.getUint32(i, true)), [0, bytes.length, 4096, 4]);
      return 0;
    },
    transport: {
      async read() { if (!frames.length) throw new Error("disconnected"); return frames.shift(); },
      async write(data) { acknowledgements.push(new DataView(data.buffer).getUint32(0, true)); },
    },
  };
}

test("full backup acknowledges bounded blocks and consumes/verifies final digest", async () => {
  const bytes = Uint8Array.from({ length: 6000 }, (_, i) => i % 256);
  const loader = stub(bytes);
  const progress = [];
  assert.deepEqual(await readFlashRegion(loader, 0, bytes.length, (p) => progress.push(p)), bytes);
  assert.deepEqual(loader.acknowledgements, [4096, 6000]);
  assert.equal(loader.frames.length, 0);
  assert.equal(progress.at(-1), 1);
});

test("truncated, corrupt, and disconnected backup never returns a recovery image", async () => {
  const bytes = new Uint8Array(6000);
  for (const frames of [[bytes.slice(0, 100)], [bytes.slice(0, 4096)],
    [bytes.slice(0, 4096), bytes.slice(4096), new Uint8Array(16)]]) {
    await assert.rejects(readFlashRegion(stub(bytes, frames), 0, bytes.length));
  }
});

function fullStub(bytes, failures = 0, corruptSnapshot = false) {
  let frames = [], reads = 0;
  const loader = {
    ESP_READ_FLASH: 0xd2,
    async checkCommand(_name, _command, payload) {
      reads++;
      if (reads <= failures) throw new Error("USB packet dropped");
      const view = new DataView(payload.buffer);
      const offset = view.getUint32(0, true), length = view.getUint32(4, true);
      const data = bytes.slice(offset, offset + length);
      frames = [];
      for (let i = 0; i < data.length; i += 4096) frames.push(data.slice(i, i + 4096));
      frames.push(new Uint8Array(createHash("md5").update(data).digest()));
      return 0;
    },
    async flashMd5sum(offset, size) {
      assert.equal(offset, 0); assert.equal(size, bytes.length);
      return corruptSnapshot ? "00".repeat(16) : createHash("md5").update(bytes).digest("hex");
    },
    transport: { async read() { return frames.shift(); }, async write() {} },
  };
  return loader;
}

test("full backup retries a damaged chunk and verifies the whole snapshot", async () => {
  const bytes = Uint8Array.from({ length: 300000 }, (_, i) => i % 256);
  const loader = fullStub(bytes, 1);
  let reconnects = 0;
  assert.deepEqual(await readFlashBackup(loader, bytes.length, () => {}, async () => {
    reconnects++; return loader;
  }), bytes);
  assert.equal(reconnects, 1);
});

test("backup retries are bounded and changed snapshots are rejected", async () => {
  const bytes = new Uint8Array(8000);
  const loader = fullStub(bytes, 3);
  let reconnects = 0;
  await assert.rejects(readFlashBackup(loader, bytes.length, () => {}, async () => {
    reconnects++; return loader;
  }));
  assert.equal(reconnects, 2);
  await assert.rejects(readFlashBackup(fullStub(bytes, 0, true), bytes.length, () => {}, async () => {}), /changed during/);
});

test("native USB reset deasserts BOOT and pulses RESET", async () => {
  const signals = [];
  await new UsbHardReset({ device: { setSignals: async (value) => signals.push(value) } }).reset();
  assert.deepEqual(signals, [{ dataTerminalReady: false, requestToSend: true },
    { dataTerminalReady: false, requestToSend: false }]);
});
