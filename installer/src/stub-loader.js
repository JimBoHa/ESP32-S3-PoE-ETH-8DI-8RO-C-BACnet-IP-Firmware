// SPDX-License-Identifier: 0BSD
import stub from "esp-flasher-stub/esp32s3.json" with { type: "json" };

export function decodeBase64Data(value) {
  return Uint8Array.from(atob(value), (byte) => byte.charCodeAt(0));
}

// Use Espressif's maintained, permissively licensed S3 stub instead of the
// legacy binaries bundled with esptool-js. Only the supported chip is loaded.
export async function getStubJsonByChipName(chipName) {
  if (chipName !== "ESP32-S3") {
    throw new Error("Wrong chip detected. This installer requires an ESP32-S3.");
  }
  return {
    entry: stub.entry, text_start: stub.text_start, data_start: stub.data_start,
    bss_start: stub.bss_start, text: stub.text, data: stub.data,
    decodedText: decodeBase64Data(stub.text), decodedData: decodeBase64Data(stub.data),
  };
}
