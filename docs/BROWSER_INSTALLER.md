# Browser installation and release publishing

## What you need

- The **ESP32-S3-POE-ETH-8DI-8RO-C**, with 16 MB flash and 8 MB PSRAM.
- A desktop computer running Chrome or Edge with Web Serial support. Safari,
  Firefox, and mobile browsers are not supported for USB flashing.
- A USB-C **data** cable. Connect Ethernet to a network with DHCP; PoE can keep
  the device powered after USB setup finishes.

![Supported Waveshare PoE + CAN controller](../installer/public/board-reference.jpg)

Photo: [Waveshare product reference](https://www.waveshare.com/esp32-s3-eth-8di-8ro-c.htm).
Match the entire label, including **POE** and **-C**. Chip detection checks
ESP32-S3 and 16 MB flash; it cannot identify the enclosure or relay wiring.

## First installation

1. Disconnect controlled relay loads. Connect USB-C and open the
   [USB installer](https://JimBoHa.github.io/ESP32-S3-PoE-ETH-8DI-8RO-C-BACnet-IP-Firmware/).
2. Click **Connect controller**. Select **USB JTAG/serial debug unit** and
   approve the browser's serial connection.
3. Confirm the board model. If you want a recovery image of the current
   firmware and settings, choose **Download backup**. The full 16 MB image
   includes private data; keep it private. Wait for the completed download.
4. Select the recommended release and acknowledge the erase. Click
   **Erase & install firmware**. Keep USB connected and the tab open.
5. The installer checks the download's SHA-256, erases flash, writes the
   merged image, verifies flash with MD5, and checks the running firmware's
   project and version. Wait for **Firmware installed**.
6. Click **Download admin key**. Confirm that the `.key` file reached your
   Downloads folder; keep a private recovery copy. Click **Finish USB setup**
   to lock key export and release the serial port.
7. Click **Open device**. In its management page, choose **Load key file** and
   select the downloaded key when you need to make changes. The file is read
   locally; only signed management requests leave the browser.

If PoE or suitable external power remains connected, you may disconnect USB
after setup finishes. If USB is the only supply, removing it turns the device
off. The installer shows relay-controller and RTC health; commissioning still
requires the site's BACnet, input, and relay checks.

## Recover a key without erasing settings

With firmware 0.14.0 or newer, connect USB and choose **Connect controller**.
Existing firmware is detected without erasing or resetting it. If key export
is locked, **hold BOOT for three seconds while firmware is running, then
release it**. Download the key within 60 seconds and finish USB setup.

Do not press RESET during this recovery step. BOOT held across RESET enters
the ROM downloader instead. If that happens, release BOOT and tap RESET.

A newly generated key can be downloaded for five minutes during its first
boot. A reset, power cycle, or **Finish USB setup** closes that initial window.
The key does not change during recovery or an Ethernet update. If a key is
compromised, an intentional erase/reinstall generates a replacement and
removes the old settings.

## Connection help

| Symptom | Next step |
|---|---|
| USB button unavailable | Open the HTTPS installer in desktop Chrome/Edge. |
| Empty device picker | Try a known data cable and another USB port; close serial monitors and other installer tabs. Allow the USB accessory if macOS prompts. |
| Cannot enter flashing mode | Keep USB connected. Hold **BOOT**, tap **RESET**, then release **BOOT**. Connect again. |
| Backup fails | Nothing has been erased. Disconnect and reconnect in the installer, then retry. Incomplete or checksum-failing backups are never offered for download. |
| Flash transfer interrupted | Reconnect and repeat installation. A partially written image may not boot, but the ROM USB downloader remains available. |
| Flash verified, USB setup interrupted | Disconnect and reconnect in the installer. A successful flash does not need another erase; use BOOT key recovery if the first-boot window expired. |
| No Ethernet address | Check Ethernet, DHCP, switch VLAN, and PoE. The page keeps polling USB and shows the MAC for DHCP lease lookup. |

The browser can save a complete flash backup, but this installer deliberately
accepts only approved release images for writing. Restoring a private full
backup is an advanced recovery operation: use esptool to write it at `0x0`,
following [commissioning and recovery](COMMISSIONING.md), or ask a maintainer.
That restores the old firmware, settings, and key together.

## Later firmware updates

Download **Ethernet update** from the installer or `firmware-ota.bin` from a
GitHub release. Open the device's **Firmware** tab, load the saved admin key,
choose the app-only image, and upload. Settings and key are preserved. Never
upload `initial-flash.bin` or a full backup through Ethernet OTA.
After the device returns online, refresh its management page and load the key
file again before issuing more commands.

## Maintainer: publish without a command line

After merging this feature:

1. Open repository **Settings → Pages**. Set **Build and deployment → Source**
   to **GitHub Actions**. Ensure the `github-pages` environment permits your
   stable release tags (for example `v*`), not only `main`.
2. Open **Releases → Draft a new release**. Choose the tested commit and a tag
   matching `PROJECT_VER`, starting with **v0.14.0**. Keep draft/prerelease
   builds unpublished until approved. Publish a stable release to promote it.
3. Watch **Actions → Publish browser installer**. It builds the tagged
   firmware with ESP-IDF 5.5.4, runs host/browser tests, verifies packages,
   attaches `bacnet-io-vX.Y.Z.tar.gz`, and deploys the HTTPS installer.
4. Open the installer link from the README and confirm the recommended
   version. Test on a spare controller before recommending a release to users.

The release workflow retains up to four older stable releases that have
compatible installer packages. It does not overwrite an existing release
asset; rerunning uses that original verified package. Use a new version for
changed firmware. Drafts, prereleases, and versions before 0.14.0 are excluded.
To recommend a prior release again, open **Actions → Publish browser installer
→ Run workflow**, run from `main`, and enter its published tag.

Pull requests only build downloadable preview artifacts. They do not publish
GitHub Pages or create a release. No PAT is embedded in the installer or
workflow; publishing uses GitHub's scoped workflow token. GitHub Pages setup
and a real public deployment must be verified after merge.

## Local development and tests

Build firmware using the repository's ESP-IDF instructions, then:

```sh
python tools/package_release.py
python tools/prepare_installer.py --release-dir release/v0.14.0 --recommended 0.14.0
cd installer
npm ci
npm test
npx playwright install chromium
npm run test:browser
npm run build
npm run preview
```

Use the localhost preview URL for physical USB testing. A `file://` page or
ordinary HTTP remote host does not provide the required secure context.
Relative assets support GitHub Pages' repository subpath without a custom
domain or runtime CDN. Serve the complete `installer/dist/` directory.

Automated browser tests substitute the device adapter with an isolated test
fixture; firmware integrity, protocol, and policy tests run separately. These
tests do not substitute for a physical USB backup/flash/boot/key test. The
production bundle contains no simulated devices or test credentials.

## USB setup protocol and implementation

The browser uses [Espressif esptool-js](https://github.com/espressif/esptool-js)
for ROM discovery, stub loading, erase, flash, and verification. Version 0.6.1
is pinned. The adapter uses a bounded, linear-time SLIP packet decoder,
supplies an actual RTS reset pulse, and implements the
documented stub backup read with at most four unacknowledged blocks. Backups
use 256 KiB chunks, bounded retries, per-chunk MD5, and a final whole-flash MD5
check; damaged or inconsistent snapshots are never downloaded. The upstream
bulk-read default lost data on native USB during bench testing.

The running firmware accepts bounded lines on native USB Serial/JTAG:

```text
BACNET-USB/1 0123456789abcdef STATUS
BACNET-USB/1 0123456789abcdef KEY
BACNET-USB/1 0123456789abcdef LOCK
```

Replies begin on a fresh line with `BACNET-USB/1 ` followed by JSON containing
`id`, `protocol: 1`, and `ok`. `STATUS` includes firmware identity, Ethernet MAC
and address, hostname, device instance, RTC/relay health, and export-window
state. `KEY` adds `admin_key` only inside a physically authorized window;
otherwise it returns `error: "key_locked"`. `LOCK` closes the window.

The parser requires an exact lowercase 16-hex request ID and known command,
accepts LF/CRLF, and discards invalid/oversized input through the next newline.
Key replies go only to native USB, never HTTP, UART logs, browser storage,
analytics, or URLs. See [security model](SECURITY.md) for physical access and
trusted-management-network assumptions.
