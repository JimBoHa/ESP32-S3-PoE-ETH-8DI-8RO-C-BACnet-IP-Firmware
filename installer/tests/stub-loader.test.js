import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { getStubJsonByChipName } from "../src/stub-loader.js";

test("maintained S3 RAM loader matches the pinned upstream release", async () => {
  const stub = await getStubJsonByChipName("ESP32-S3");
  assert.equal(stub.entry, 1077384484);
  assert.ok(stub.entry >= stub.text_start && stub.entry < stub.text_start + stub.decodedText.length);
  assert.equal(createHash("sha256").update(stub.decodedText).digest("hex"),
    "c37a7d12d52633fd629fbed13f3a7418216af368d8e80f17853ce6de29741374");
  assert.ok(stub.decodedData.length > 0);
  await assert.rejects(getStubJsonByChipName("ESP32"), /Wrong chip/);
});
