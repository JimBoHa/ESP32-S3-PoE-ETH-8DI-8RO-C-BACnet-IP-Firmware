# Windows and Mac compatibility

The browser installer uses Web Serial and targets current desktop Chrome and
Edge on Windows and macOS. Chrome documents Web Serial support on both
[Windows and macOS](https://developer.chrome.com/docs/capabilities/serial#browser-support).
Safari, Firefox, and mobile browsers are not supported by this USB installer.
The same firmware package and setup page are used on both platforms.

## Automated browser evidence

On 2026-09-07, [GitHub Actions run 34170923472](https://github.com/JimBoHa/ESP32-S3-PoE-ETH-8DI-8RO-C-BACnet-IP-Firmware/actions/runs/34170923472)
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

The Windows browser hardware test on 2026-09-07 used **Edge 152.0.4191.66** on
Windows 11 Home Single Language 25H2 ARM64 (build 26200.8037), in UTM 4.7.5
with the real controller attached through USB passthrough. Windows enumerated
Espressif VID `303A`, PID `1001` as `USB Serial Device (COM3)`, using Microsoft's
signed USB serial driver version 10.0.26100.4202. No third-party serial driver
was needed for this controller in this VM.

| Hardware check through Windows Edge | Result |
|---|---|
| Full 16,777,216-byte browser backup | Passed; chunk and whole-flash MD5 verified, downloaded-file SHA-256 matched after copying to macOS |
| Erase, write, flash verification, and USB boot handshake | Passed with firmware 0.14.0 |
| First-boot key download and explicit export lock | Passed; a new key was saved privately and subsequent export was denied |
| Running-device connect and disconnect | Passed without a reset, including after installation and Ethernet OTA |
| Installer Ethernet discovery and **Open device** link | Passed; the link opened the real management interface |
| Key-file loading, signed Ethernet OTA, and signed reboot | Passed; the other OTA slot booted and the same key authorized a subsequent reboot |
| Controller health and 26 read-only BACnet checks | Passed after installation and OTA/reboot; inputs and relay commands were inactive |

This test exposed a Windows disconnect reset. The installer now clears RTS
before DTR, in separate calls, before closing USB setup. The corrected sequence
passed the physical checks above and a regression test that models Windows
clearing DTR before RTS. A second regression test verifies port cleanup after
signal-control failure. All 22 JavaScript unit tests, 15 Chromium browser tests,
and the production installer build passed locally.

Playwright drove an isolated Edge profile in the normal Windows user session.
The real USB port was preauthorized only for the loopback installer origin and
the device's exact VID/PID, using Microsoft's
[SerialAllowUsbDevicesForUrls policy](https://learn.microsoft.com/en-us/deployedge/microsoft-edge-policies/serialallowusbdevicesforurls).
The policy was restored after each run. Only port selection was substituted;
all serial transfers, firmware replies, downloads, and Ethernet requests used
the real browser and controller. Native Windows device/file chooser dialogs and Chrome were excluded from
that 0.14.0 run. The expanded 1.0.0 results below supersede those gaps. Physical
BOOT-button recovery and public GitHub Pages deployment remain untested.

## Version 1.0 release validation

Complete 1.0.0 hardware runs passed in **Chrome 152.0.7977.83** and
**Edge 152.0.4191.66** on September 7, 2026 (Pacific time). Both ran in the
Windows 11 Home Single Language 25H2 **ARM64** VM with the real controller
attached through UTM USB passthrough. Final Windows inventory reported build
26200.9168 and Microsoft's signed USB serial driver 10.0.26100.8521; the VM
received Windows servicing updates after the earlier 0.14.0 baseline above.

| Check for each Windows installation run | Chrome | Edge |
|---|---|---|
| Native USB chooser: cancel, select controller, and connect | Passed | Passed |
| Locked key export denied before installation | Passed | Passed |
| Full 16,777,216-byte backup with chunk/whole-flash MD5 and copied-file SHA-256 verification | Passed | Passed |
| Erase, write, flash verification, and verified 1.0.0 USB boot | Passed | Passed |
| New key download, export locking, and Ethernet discovery/handoff | Passed | Passed |
| Native key and OTA file dialogs: cancel and select a real file | Passed | Passed |
| Running-device reconnect/disconnect without a reset | Passed | Passed |
| Signed Ethernet OTA: alternate slot boots, settings and key retained | Passed | Passed |
| Same key authorizes a subsequent reboot | Passed | Passed |
| Final health and 26 read-only BACnet property checks | Passed | Passed |

Both final runs used fresh, isolated browser profiles in the normal Windows
user session. No Web Serial override, file-input substitution, or serial
preauthorization policy was used. Microsoft UI Automation operated the real
USB and file dialogs. The native filename textbox was filled through its
Win32 control API; the browser read the selected file. The test helper was
made DPI-aware and brought file dialogs to the foreground before operating
native controls. Post-run BACnet property checks used a separate macOS bench
client; the Windows browsers checked health through the controller's HTTP API.
The final snapshots reported all eight inputs and relay
commands inactive, with Ethernet, BACnet, RTC, and relay-controller health good.

Windows testing exposed dropped USB bytes with esptool-js's bundled legacy
RAM loader. Changing its transfer window or baud rate did not fix the failure.
The installer now pins Espressif's maintained **esp-flasher-stub 1.2.2** S3
loader under its MIT license option. This loader passed the same diagnostic
reads at both 115200 and 460800 baud, followed by the complete runs above.
Per-chunk and whole-flash integrity checks and bounded retries remain enabled;
failed backups are rejected before any erase. Earlier failed or interrupted
runs are retained privately and are not counted as passes.

The 1.0.0 ESP-IDF 5.5.4 build, release package verification, two C test programs,
45 Python tests, 23 JavaScript tests, 15 local Chromium browser tests,
production installer build, and workflow lint passed. CI also passed all five
jobs, including 15 tests each in native Windows x64 and macOS Chrome and Edge.
The initial image is 718,208 bytes; the OTA image is 587,136 bytes. Packaged
ZIP and TAR files contain identical tested firmware images and notices.

**Coverage limit:** a native physical x64 Windows PC has not flashed the
controller. The ARM64 VM exercises the real Windows USB stack, while x64 CI
exercises browser APIs and fixture-based UI flows without USB hardware. These
are distinct checks. Public installer deployment, physical BOOT-button key
recovery, and the broader field/endurance suite also remain separate checks.

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

The generated page captures use a fresh, isolated browser context. They exclude
the desktop, browser tabs/profile, usernames, personal files, and real controller
identifiers. Network and device values are examples; the example admin key
remains masked. Separate native Windows captures show only the selected USB
dialog or relevant file-dialog controls from the dedicated test environment;
see [native-captures.json](images/native-captures.json). No key contents or
personal folders are included. Published PNGs contain no embedded text, EXIF,
or location metadata. Preserve these privacy properties when refreshing them.
