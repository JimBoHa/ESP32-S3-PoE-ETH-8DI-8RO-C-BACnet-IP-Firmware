// SPDX-License-Identifier: Apache-2.0
// Linear-time SLIP decoding for esptool-js 0.6.1's Transport buffer. Upstream
// appends a new Uint8Array for every byte; full-flash backups magnify that cost.
// The pinned Transport continues to own the Web Serial reader and disconnect.
export async function readSlipPacket(transport, timeout) {
  let packet = new Uint8Array(4096);
  let length = 0, started = false, escaping = false;
  let deadline = Date.now() + timeout;
  for (;;) {
    while (!transport.buffer.length) {
      if (Date.now() >= deadline) throw new Error("Serial transfer timed out. Check USB and reconnect.");
      await new Promise((resolve) => setTimeout(resolve, 1));
    }
    const incoming = transport.buffer;
    transport.buffer = new Uint8Array(0);
    deadline = Date.now() + timeout;
    for (let i = 0; i < incoming.length; i++) {
      let byte = incoming[i];
      if (!started) {
        if (byte !== 0xc0) throw new Error("Invalid serial packet. Disconnect and reconnect to retry.");
        started = true;
        continue;
      }
      if (escaping) {
        if (byte !== 0xdc && byte !== 0xdd) throw new Error("Invalid serial escape. Reconnect to retry.");
        byte = byte === 0xdc ? 0xc0 : 0xdb;
        escaping = false;
      } else if (byte === 0xdb) {
        escaping = true;
        continue;
      } else if (byte === 0xc0) {
        transport.buffer = incoming.slice(i + 1);
        return packet.slice(0, length);
      }
      if (length === packet.length) {
        if (packet.length >= 65536) throw new Error("Serial packet exceeded the supported size.");
        const larger = new Uint8Array(packet.length * 2);
        larger.set(packet); packet = larger;
      }
      packet[length++] = byte;
    }
  }
}
