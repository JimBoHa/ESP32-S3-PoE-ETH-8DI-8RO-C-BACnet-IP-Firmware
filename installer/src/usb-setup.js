// SPDX-License-Identifier: 0BSD
export const PREFIX = "BACNET-USB/1 ";

export class SetupClient {
  constructor(port) {
    this.port = port;
    this.pending = new Map();
    this.buffer = "";
    this.closed = true;
  }

  async open() {
    if (!this.port.readable) await this.port.open({ baudRate: 115200, bufferSize: 8192 });
    this.closed = false;
    this.reader = this.port.readable.getReader();
    this.reading = this.readLoop();
  }

  async readLoop() {
    const decoder = new TextDecoder();
    try {
      while (!this.closed) {
        const { value, done } = await this.reader.read();
        if (done) break;
        this.consume(decoder.decode(value, { stream: true }));
      }
    } catch {
      // Never include raw serial data here: KEY replies contain a credential.
    } finally {
      this.reader.releaseLock();
      this.reader = null;
      for (const entry of this.pending.values()) {
        clearTimeout(entry.timer);
        entry.reject(new Error("USB disconnected. Reconnect the controller to continue setup."));
      }
      this.pending.clear();
    }
  }

  consume(text) {
    this.buffer += text;
    let end;
    while ((end = this.buffer.indexOf("\n")) !== -1) {
      const line = this.buffer.slice(0, end).replace(/\r$/, "");
      this.buffer = this.buffer.slice(end + 1);
      if (!line.startsWith(PREFIX) || line.length > 4096) continue;
      let response;
      try { response = JSON.parse(line.slice(PREFIX.length)); } catch { continue; }
      const entry = this.pending.get(response?.id);
      if (!entry || response.protocol !== 1) continue;
      clearTimeout(entry.timer);
      this.pending.delete(response.id);
      if (response.ok) entry.resolve(response);
      else entry.reject(new Error(response.error === "key_locked"
        ? "Hold BOOT for 3 seconds while the device is running, release it, then download the key again."
        : "Controller could not complete USB setup. Reconnect and try again."));
    }
    if (this.buffer.length > 8192) this.buffer = "";
  }

  async request(command, timeout = 2500) {
    if (!["STATUS", "KEY", "LOCK"].includes(command)) throw new Error("Unsupported setup command.");
    if (this.closed || !this.port.writable) throw new Error("Connect the controller first.");
    const id = [...crypto.getRandomValues(new Uint8Array(8))]
      .map((byte) => byte.toString(16).padStart(2, "0")).join("");
    let entry;
    const response = new Promise((resolve, reject) => {
      entry = { resolve, reject, timer: setTimeout(() => {
        this.pending.delete(id);
        reject(new Error("USB setup did not respond. Check the cable and reconnect."));
      }, timeout) };
      this.pending.set(id, entry);
    });
    // Attach the rejection handler before awaiting a potentially slow write.
    response.catch(() => {});
    const writer = this.port.writable.getWriter();
    try {
      await writer.write(new TextEncoder().encode(`${PREFIX}${id} ${command}\n`));
    } catch (error) {
      clearTimeout(entry.timer);
      this.pending.delete(id);
      entry.reject(error);
    } finally {
      writer.releaseLock();
    }
    return response;
  }

  async close() {
    this.closed = true;
    if (this.reader) await this.reader.cancel().catch(() => {});
    await this.reading;
    if (this.port.readable || this.port.writable) {
      try {
        // Windows may clear DTR before RTS when closing the port. DTR=0
        // with RTS=1 triggers an ESP32-S3 reset through USB Serial/JTAG.
        // Clear RTS first, in a separate call, to avoid that intermediate
        // state even when the driver applies signal changes sequentially.
        await this.port.setSignals({ requestToSend: false });
        await this.port.setSignals({ dataTerminalReady: false });
      } finally {
        await this.port.close();
      }
    }
    this.buffer = "";
  }
}
