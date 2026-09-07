// SPDX-License-Identifier: Apache-2.0
import { Controller } from "./device.js";
import { MODEL, assetURL, validateCatalog, verifyImage, deviceURL, sha256 } from "./firmware.js";

const $ = (id) => document.getElementById(id);
const catalogURL = new URL("firmware/catalog.json", document.baseURI);
let catalog, controller, busy = false, keySaved = false, flashed = false;
let polling, refreshing, status;
const supported = Boolean(navigator.serial && window.isSecureContext);
$('unsupported').hidden = supported;
$('connect').disabled = !supported;

function message(text, kind = "") {
  $('message').textContent = text;
  $('message').className = `notice ${kind}`;
  $('message').hidden = !text;
}

function step(current) {
  ["connect", "install", "key", "network"].forEach((name, index) => {
    const element = $(`step-${name}`);
    element.classList.toggle("complete", index < current);
    if (index === current) element.setAttribute("aria-current", "step");
    else element.removeAttribute("aria-current");
  });
}

function controls() {
  $('connect').disabled = busy || !supported || Boolean(controller);
  $('disconnect').hidden = !controller;
  $('disconnect').disabled = busy;
  $('version').disabled = busy || !catalog;
  $('board-confirm').disabled = busy;
  $('erase-confirm').disabled = busy;
  $('backup').disabled = busy || !controller || !$('board-confirm').checked;
  $('install').disabled = busy || !controller || !catalog || !$('board-confirm').checked || !$('erase-confirm').checked;
  $('download-key').disabled = busy || !controller || !status;
  $('finish').disabled = busy || !controller || !keySaved;
}

function progress(phase, amount) {
  const labels = { backup: "Reading recovery backup", erase: "Erasing controller", write: "Writing firmware", verified: "Flash contents verified", download: "Checking firmware package", boot: "Waiting for firmware startup" };
  $('progress-region').hidden = false;
  $('progress-label').textContent = labels[phase] || "Working";
  $('progress-percent').textContent = phase === "write" || phase === "backup" ? `${Math.round(amount * 100)}%` : "";
  if (phase === "erase" || phase === "boot" || phase === "download") $('progress').removeAttribute("value");
  else $('progress').value = amount;
}

function release() {
  return catalog?.releases.find((item) => item.version === $('version').value);
}

function saveFile(data, name, type = "application/octet-stream") {
  const url = URL.createObjectURL(new Blob([data], { type }));
  const link = document.createElement("a");
  link.href = url; link.download = name;
  document.body.append(link); link.click(); link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function updateStatus(next) {
  status = next;
  $('setup-options').hidden = false;
  $('unlock-help').hidden = next.key_export_allowed || keySaved;
  $('health').hidden = false;
  $('health-version').textContent = next.firmware_version;
  $('health-relays').textContent = next.relay_controller_healthy ? "Healthy" : "Fault reported";
  $('health-rtc').textContent = next.rtc_present ? "Present" : "Not detected";
  $('health-instance').textContent = String(next.device_instance);
  const url = next.ethernet_link ? deviceURL(next.ip_address) : null;
  $('open-device').hidden = !url;
  if (url) $('open-device').href = url;
  else $('open-device').removeAttribute("href");
  $('network-dot').classList.toggle("ready", Boolean(url));
  $('network-title').textContent = url ? "Ethernet connected" : next.ethernet_link ? "Waiting for network address" : "Connect Ethernet";
  $('network-detail').textContent = url ? `${next.ip_address} · ${next.hostname}`
    : `Connect to a network with DHCP. Ethernet MAC: ${next.ethernet_mac}`;
  $('health-warning').hidden = next.relay_controller_healthy && next.rtc_present;
  controls();
}

function startPolling() {
  clearInterval(polling);
  polling = setInterval(() => {
    if (busy || refreshing || !controller?.setup) return;
    refreshing = controller.status().then(updateStatus).catch(() => {
      clearInterval(polling);
      message("USB setup disconnected. Reconnect here to continue; reinstalling is not required.", "warning");
    }).finally(() => { refreshing = null; });
  }, 1500);
}

async function run(action) {
  if (busy) return;
  busy = true; clearInterval(polling); controls(); message("");
  try { await refreshing; await action(); }
  catch (error) {
    $('progress-region').hidden = true;
    const text = error.name === "NotFoundError" ? "No device selected. Click Connect controller when ready."
      : error.message || "Connection failed. Check the USB cable and try again.";
    message(text, error.name === "NotFoundError" ? "" : "error");
  } finally {
    busy = false; controls();
    if (controller?.setup && status) startPolling();
  }
}

$('connect').addEventListener("click", () => run(async () => {
  const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a, usbProductId: 0x1001 }] });
  const candidate = new Controller(port, progress);
  $('phase-label').textContent = "CONNECTING";
  $('workspace-title').textContent = "Checking your controller";
  try {
    const identity = await candidate.connect();
    controller = candidate; keySaved = false; flashed = false;
    $('connection-state').textContent = "USB connected";
    $('connection-state').className = "pill connected";
    $('workspace-title').textContent = "Controller connected";
    $('phase-label').textContent = "READY";
    $('device-identity').textContent = `${identity.chip} · ${identity.mac}`;
    $('device-identity').hidden = false;
    $('install-options').hidden = false;
    $('intro').textContent = `Match the board label to ${MODEL} before installing.`;
    $('key-state').textContent = "REQUIRED FOR MANAGEMENT";
    step(identity.running ? 2 : 1);
    if (identity.running) {
      updateStatus(identity.running);
      $('existing-firmware').hidden = false;
      $('existing-firmware').textContent = `Firmware ${identity.running.firmware_version} is already running. You can save its key or open the device below. Routine updates use the device’s Firmware tab.`;
    } else {
      $('existing-firmware').hidden = true;
      $('setup-options').hidden = true;
      message("Controller detected. Confirm its model, then install the approved firmware.");
    }
  } catch (error) {
    await candidate.disconnect().catch(() => {});
    $('phase-label').textContent = "CONNECTION HELP";
    $('workspace-title').textContent = "Try connecting again";
    throw error;
  }
}));

$('board-confirm').addEventListener("change", controls);
$('erase-confirm').addEventListener("change", controls);
$('version').addEventListener("change", () => {
  const current = release();
  $('download-ota').href = assetURL(current.ota.path, catalogURL);
  $('download-ota').download = `bacnet-io-${current.version}-ota.bin`;
});

$('backup').addEventListener("click", () => run(async () => {
  const bytes = await controller.backup();
  const digest = await sha256(bytes);
  const mac = controller.identity.mac.replaceAll(":", "");
  saveFile(bytes, `bacnet-io-${mac}-full-backup-${digest.slice(0, 12)}.bin`);
  $('progress-region').hidden = true;
  message("Full 16 MB recovery backup downloaded. It includes existing settings and may contain private keys. Keep it private.", "success");
}));

$('install').addEventListener("click", () => run(async () => {
  const selected = release();
  progress("download", 0);
  const response = await fetch(assetURL(selected.initial.path, catalogURL), { cache: "no-store", credentials: "omit" });
  if (!response.ok) throw new Error("Firmware download failed. Nothing has been erased. Reload and try again.");
  const bytes = await verifyImage(new Uint8Array(await response.arrayBuffer()), selected.initial);
  status = null; keySaved = false;
  $('setup-options').hidden = true;
  await controller.install(bytes);
  flashed = true;
  progress("boot", 0);
  let boot;
  try { boot = await controller.startApplication(selected.version); }
  catch {
    throw new Error("Firmware was written and verified, but USB setup did not reconnect. Disconnect and reconnect here to finish setup; you do not need to erase again.");
  }
  updateStatus(boot);
  $('progress-region').hidden = true;
  $('workspace-title').textContent = "Firmware installed";
  $('phase-label').textContent = "WRITE & BOOT VERIFIED";
  $('erase-confirm').checked = false;
  $('key-state').textContent = "DOWNLOAD YOUR NEW KEY";
  step(2);
  message(`Firmware ${selected.version} installed and started. Save your admin key below.`, "success");
}));

$('download-key').addEventListener("click", () => run(async () => {
  const key = await controller.key();
  const mac = status.ethernet_mac.replaceAll(":", "");
  saveFile(`${key}\n`, `bacnet-io-${mac}-admin.key`, "text/plain");
  keySaved = true;
  $('key-state').textContent = "KEY DOWNLOAD STARTED";
  $('unlock-help').hidden = true;
  step(3);
  message("Admin key download started. Confirm the file is saved before finishing USB setup. Use “Load key file” in the device’s web interface.", "success");
}));

async function disconnect() {
  clearInterval(polling);
  await controller?.disconnect();
  controller = null; status = null;
  $('connection-state').textContent = "USB disconnected";
  $('connection-state').className = "pill";
  $('install-options').hidden = true;
  $('device-identity').hidden = true;
  $('progress-region').hidden = true;
}

$('disconnect').addEventListener("click", () => run(async () => {
  await disconnect();
  $('setup-options').hidden = true;
  $('workspace-title').textContent = "Connect your controller";
  $('phase-label').textContent = "GET STARTED";
  step(0); message("USB disconnected. The controller can continue running on PoE or external power.");
}));

$('finish').addEventListener("click", () => run(async () => {
  await controller.lock();
  await disconnect();
  $('workspace-title').textContent = "USB setup complete";
  $('phase-label').textContent = "READY TO COMMISSION";
  message(`${flashed ? "Firmware installed. " : ""}Key export locked. USB can be disconnected; keep PoE or external power connected to continue using the controller.`, "success");
}));

window.addEventListener("beforeunload", (event) => {
  if (busy) { event.preventDefault(); event.returnValue = ""; }
});

try {
  const response = await fetch(catalogURL, { cache: "no-store", credentials: "omit" });
  if (!response.ok) throw new Error("No approved firmware package is published yet. Existing-device USB setup is still available.");
  catalog = validateCatalog(await response.json(), catalogURL);
  $('version').replaceChildren(...catalog.releases.map((item) => {
    const option = document.createElement("option");
    option.value = item.version;
    option.textContent = `${item.version}${item.version === catalog.recommended ? " — Recommended" : " — Previous release"}`;
    option.selected = item.version === catalog.recommended;
    return option;
  }));
  const selected = release();
  $('download-ota').href = assetURL(selected.ota.path, catalogURL);
  $('download-ota').download = `bacnet-io-${selected.version}-ota.bin`;
  $('download-ota').hidden = false;
} catch (error) {
  message(error.message, "warning");
  $('version').replaceChildren(new Option("No approved release available", ""));
}
controls();
