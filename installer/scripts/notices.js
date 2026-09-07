import { readFile, writeFile } from "node:fs/promises";

const parts = ["BACnet IO browser installer — third-party licenses\n"];
for (const name of ["esptool-js", "pako", "spark-md5"]) {
  const root = new URL(`../node_modules/${name}/`, import.meta.url);
  const metadata = JSON.parse(await readFile(new URL("package.json", root), "utf8"));
  parts.push(`\n${name} ${metadata.version} (${metadata.license})\n`,
    await readFile(new URL("LICENSE", root), "utf8"));
}
for (const file of ["deflate.js", "inflate.js"]) {
  const source = await readFile(new URL(`../node_modules/pako/lib/zlib/${file}`, import.meta.url), "utf8");
  const notice = source.match(/\/\/ \(C\)[\s\S]+?(?=\n\n)/)?.[0];
  if (!notice?.includes("This notice may not be removed")) throw new Error(`Missing zlib notice in ${file}`);
  parts.push(`\npako zlib/${file}\n`, notice);
}
parts.push("\nBoard reference photograph: © Waveshare. Used to identify the supported product; excluded from the project's Apache-2.0 license.\nSource: https://www.waveshare.com/img/devkit/accBoard/ESP32-S3-ETH-8DI-8RO-C/ESP32-S3-ETH-8DI-8RO-C-details-2-2.jpg\n");
await writeFile(new URL("../public/licenses.txt", import.meta.url), parts.join("\n"));
