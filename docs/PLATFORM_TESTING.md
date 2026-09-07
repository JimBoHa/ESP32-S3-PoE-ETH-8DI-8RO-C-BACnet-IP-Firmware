# Windows and Mac compatibility

The browser installer uses Web Serial and targets current desktop Chrome and
Edge on Windows and macOS. Chrome documents Web Serial support on both
[Windows and macOS](https://developer.chrome.com/docs/capabilities/serial#browser-support).
Safari, Firefox, and mobile browsers are not supported by this USB installer.
The same firmware package and setup page are used on both platforms.

## Automated browser evidence

On 2026-09-07, [GitHub Actions run 34154925507](https://github.com/JimBoHa/ESP32-S3-PoE-ETH-8DI-8RO-C-BACnet-IP-Firmware/actions/runs/34154925507)
passed these desktop-browser checks:

| Runner | Browser | Result |
|---|---|---|
| Windows Server 2025, x64 | Chrome 152.0.7977.83 | 15 browser tests passed |
| Windows Server 2025, x64 | Edge 152.0.4191.66 | 15 browser tests passed |
| macOS 26, ARM64 | Chrome 152.0.7977.83 | 15 browser tests passed |
| macOS 26, ARM64 | Edge 152.0.4191.66 | 15 browser tests passed |
| Linux, x64 | Chromium 153.0.8010.12 | 15 browser tests passed |

The native capability test uses the actual browser's Web Serial API, signal
control API, secure context, SHA-256, streams, file reading, and download
support. The UI tests cover installation confirmations, integrity failure,
interrupted flash/setup recovery, key-file handling, backup download, Ethernet
handoff, and layout. Device responses are isolated test fixtures for those UI
tests. GitHub-hosted runners do not have the physical controller attached.

The workflow repeats the Chrome/Edge matrix on Windows and macOS for pull
requests and `main` changes. It also runs the JavaScript unit tests and builds
the installer on each OS. Browser versions are reported in each job's log.

## Physical USB evidence

The macOS bench test on firmware 0.14.0 passed a complete browser-based 16 MB
backup, erase/flash/verification, USB boot handshake, private key download,
export locking, Ethernet discovery, and reconnect without resetting the
controller. A signed Ethernet OTA update and subsequent reboot passed, with
26 read-only BACnet checks afterward. See [development validation](DEVELOPMENT.md).

Windows browser compatibility is tested above. A Windows controller test is
still required to validate its COM-port driver, USB reset/re-enumeration,
complete 16 MB transfer, and the first-boot key window. Passing browser tests
must not be described as passing a physical Windows flash.

## Windows hardware test procedure

Use Windows 11 on a physical PC, or a Windows VM that owns the actual USB
device. A normal Docker container is not a replacement for this test:
[containers share the host kernel](https://learn.microsoft.com/en-us/virtualization/windowscontainers/about/),
and browser fixtures do not exercise a Windows USB driver. On Apple silicon,
a Windows 11 ARM64 VM with USB passthrough can test the Windows USB stack;
it does not replace a native x64-PC acceptance test.

1. Use a spare controller or obtain permission to erase it. Disconnect
   controlled relay loads and retain a private full backup and the current key.
2. For a VM, attach only the Espressif USB Serial/JTAG device to the Windows
   guest. Close host serial monitors and installer connections. Confirm the
   controller appears as a serial/COM port inside Windows.
3. Open the HTTPS installer in Windows Chrome. For a development build, serve
   `installer/dist/` on `localhost` inside the guest; an ordinary HTTP URL on
   the Mac host does not satisfy the browser's secure-context requirement.
4. Complete the [illustrated installation sequence](BROWSER_INSTALLER.md),
   including the full 16 MB backup, erase/flash verification, first-boot status,
   key download, key-export lock, and Ethernet handoff. Record the browser and
   Windows build, COM device, version, and backup/image hashes privately.
5. Reconnect without an erase. Confirm existing firmware is detected without
   a reset and key export remains locked. Hold BOOT for three seconds while
   firmware runs, release it, download the same key, and lock export again.
6. Load the new key in the Ethernet interface, perform an app-only OTA update,
   and verify the saved settings/key, network health, inputs, and safe output
   states after reboot. Repeat the USB sequence in Edge on the spare device.
7. Record which browser/architecture actually flashed the controller, any VM
   USB reattachment needed, and any untested steps. Keep backups and key files
   outside public reports and screenshots.

## Screenshot provenance

The PNGs in [images](images/) are captures of the actual installer and embedded
management interfaces, rendered with example data by
[`installer/scripts/screenshots.js`](../installer/scripts/screenshots.js).
They are instructional illustrations, not hardware acceptance evidence.
The script blocks external requests and never opens a serial device. Regenerate
them with `npm run screenshots` inside `installer/`, then inspect every image.
Capture metadata is stored in [captures.json](images/captures.json).

Published captures contain only page content from a fresh, isolated browser
context. They exclude the desktop, browser tabs/profile, native file pickers,
usernames, personal files, and real controller identifiers. All network and
device values are examples; the example admin key remains masked. The PNGs
contain only image chunks, with no embedded text, EXIF, or location metadata.
Preserve these privacy properties when refreshing the screenshots.
