import test from "node:test";
import assert from "node:assert/strict";
import { SetupClient, PREFIX } from "../src/usb-setup.js";

function fakePort(handler) {
  let source;
  return {
    readable: null, writable: null, lastSignals: null,
    async open() {
      this.readable = new ReadableStream({ start(controller) { source = controller; } });
      this.writable = new WritableStream({ write: (bytes) => handler(new TextDecoder().decode(bytes), source) });
    },
    async setSignals(signals) { this.lastSignals = signals; },
    async close() { this.readable = null; this.writable = null; },
  };
}
const encode = (text) => new TextEncoder().encode(text);

test("setup finds split responses amid boot logs and ignores unrelated request IDs", async () => {
  const port = fakePort((text, source) => {
    const id = text.split(" ")[1];
    source.enqueue(encode(`boot log\r\n${PREFIX}{"protocol":1,"id":"unrelated","ok":true}\n`));
    source.enqueue(encode(`${PREFIX}{"protocol":1,"id":"${id}",`));
    source.enqueue(encode('"ok":true,"firmware_version":"0.14.0"}\r\n'));
  });
  const client = new SetupClient(port); await client.open();
  assert.equal((await client.request("STATUS")).firmware_version, "0.14.0");
  assert.equal(port.lastSignals, null, "setup must not toggle reset/boot signals");
  await client.close();
  assert.equal(port.readable, null);
});

test("setup reuses the open flashing port without another OS open or reset", async () => {
  const port = fakePort((text, source) => {
    const id = text.split(" ")[1];
    source.enqueue(encode(`${PREFIX}{"protocol":1,"id":"${id}","ok":true}\n`));
  });
  await port.open();
  port.open = async () => { throw new Error("Unexpected port reopen"); };
  const client = new SetupClient(port);
  await client.open();
  assert.equal((await client.request("STATUS")).ok, true);
  assert.equal(port.lastSignals, null);
  await client.close();
});

test("locked key response gives physical recovery instructions without exposing serial contents", async () => {
  const port = fakePort((text, source) => {
    const id = text.split(" ")[1];
    source.enqueue(encode(`${PREFIX}{"protocol":1,"id":"${id}","ok":false,"error":"key_locked"}\n`));
  });
  const client = new SetupClient(port); await client.open();
  await assert.rejects(client.request("KEY"), /Hold BOOT for 3 seconds/);
  await client.close();
});

test("oversized and malformed serial messages do not prevent the next valid reply", async () => {
  const port = fakePort((text, source) => {
    const id = text.split(" ")[1];
    source.enqueue(encode("x".repeat(10000)));
    source.enqueue(encode(`\n${PREFIX}{invalid}\n${PREFIX}{"protocol":1,"id":"${id}","ok":true}\n`));
  });
  const client = new SetupClient(port); await client.open();
  assert.equal((await client.request("STATUS")).ok, true);
  assert.equal(client.pending.size, 0);
  await client.close();
});

test("silent port times out and can close without leaking its reader lock", async () => {
  const client = new SetupClient(fakePort(() => {})); await client.open();
  await assert.rejects(client.request("STATUS", 20), /did not respond/);
  assert.equal(client.pending.size, 0);
  await client.close();
});

test("disconnect rejects pending requests and unknown commands never reach the port", async () => {
  let writes = 0;
  const client = new SetupClient(fakePort((_text, source) => { writes++; source.close(); }));
  await client.open();
  await assert.rejects(client.request("ERASE"), /Unsupported/);
  assert.equal(writes, 0);
  await assert.rejects(client.request("STATUS"), /disconnected/);
  await client.close();
});
