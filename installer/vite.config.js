import { defineConfig } from "vite";
import { fileURLToPath } from "node:url";

export default defineConfig({
  base: "./",
  resolve: { alias: [{
    // esptool-js 0.6.1 imports this exact module from esploader.js.
    find: "./stubFlasher.js",
    replacement: fileURLToPath(new URL("./src/stub-loader.js", import.meta.url)),
  }] },
  build: { target: "es2022" },
  server: { port: 5173, strictPort: true },
});
