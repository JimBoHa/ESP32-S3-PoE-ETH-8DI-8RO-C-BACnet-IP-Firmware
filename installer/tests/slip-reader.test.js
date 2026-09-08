import test from "node:test";
import assert from "node:assert/strict";
import { readSlipPacket } from "../src/slip-reader.js";

test("SLIP unescapes data and preserves subsequent packets", async () => {
  const transport = { buffer: Uint8Array.of(0xc0, 1, 0xdb, 0xdc, 0xdb, 0xdd, 0xc0, 0xc0, 2, 0xc0) };
  assert.deepEqual(await readSlipPacket(transport, 20), Uint8Array.of(1, 0xc0, 0xdb));
  assert.deepEqual(await readSlipPacket(transport, 20), Uint8Array.of(2));
  assert.equal(transport.buffer.length, 0);
});

test("SLIP supports an escape split across incoming chunks", async () => {
  const transport = { buffer: Uint8Array.of(0xc0, 0xdb) };
  setTimeout(() => { transport.buffer = Uint8Array.of(0xdc, 0xc0); }, 5);
  assert.deepEqual(await readSlipPacket(transport, 100), Uint8Array.of(0xc0));
});

test("SLIP rejects noise, invalid escapes, missing data, and oversized packets", async () => {
  for (const buffer of [Uint8Array.of(1), Uint8Array.of(0xc0, 0xdb, 1),
    Uint8Array.of(0xc0), new Uint8Array(65538)]) {
    if (buffer.length > 65536) buffer[0] = 0xc0;
    await assert.rejects(readSlipPacket({ buffer }, 10));
  }
});

test("SLIP handles a full backup-sized stream without per-byte array copies", async () => {
  const frame = new Uint8Array(4098).fill(0xff);
  frame[0] = frame[4097] = 0xc0;
  for (let i = 0; i < 4096; i++) {
    const data = await readSlipPacket({ buffer: frame }, 20);
    assert.equal(data.length, 4096);
    assert.equal(data[4095], 0xff);
  }
});
