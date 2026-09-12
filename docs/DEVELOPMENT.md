# Development and release

For GPT-6 Astra model selection, project instructions, and scoped agent
verification, see [Codex CLI development](CODEX_WORKFLOW.md).

## Reproducible baseline

- ESP-IDF: 5.5.4
- Target: `esp32s3`
- bacnet-stack: tag `bacnet-stack-1.6.0`, commit
  `9bc3cfa07aab98852de432fa24079f4b4b6b7eed`
- Flash: 16 MB, DIO, 80 MHz
- Partition layout: two 6 MiB OTA slots with rollback enabled

Clone submodules before configuration:

```sh
git submodule update --init --recursive
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

`sdkconfig` is generated and ignored. Committed `sdkconfig.defaults` and
`partitions.csv` define the reproducible inputs. Regenerate `sdkconfig` before
a release build; an older ignored file can otherwise retain obsolete values.
The application has a compile-time assertion requiring a BACnet UDP receive
mailbox of at least 64 datagrams, and the compiled value is exposed at runtime
as `bacnet_udp_receive_mailbox_size`.

## Tests

The [browser installer guide](BROWSER_INSTALLER.md#local-development-and-tests)
documents browser tests and the release-to-Pages workflow. Browser changes
require the Node tests, Chromium interaction tests, and a production build;
USB transport changes also require a physical-controller check. A mock device
fixture cannot prove Web Serial timing, flash contents, or USB reboot behavior.

Use an up-to-date Python with `tarfile.data_filter` for the host/release tests
(Python 3.12 or newer includes it). The release collector deliberately uses
secure archive extraction; an old system Python such as macOS Python 3.9.6
does not provide that API. Put the supported interpreter's `python3` on `PATH`
before running the host suite. The ESP-IDF environment may use a different
Python interpreter.

```sh
tests/run_host_tests.sh
idf.py fullclean
idf.py build
idf.py size
git diff --check
```

Host tests cover persistent-model validation/CRC behavior; management-client
URL, key, OTA descriptor, canonical request, HMAC, and relay command behavior;
bounded BACnet command-trace filtering, response association, truncation, ring
ordering, and authenticated client routing; and static integrity of the embedded
management interface. The IDF build
compiles the real ESP32-S3 application, embedded page, and selected
bacnet-stack sources.

The host suite also runs `tests/run_bacnet_delivery_tests.py`. It compiles the
pinned BACnet COV, transaction state machine, APDU dispatcher, and BI/BO code
with AddressSanitizer and UndefinedBehaviorSanitizer. Ten isolated scenarios
run in both baseline and candidate-recovery modes, with simulated time and
an in-memory datalink; no network socket, real point, or relay is used.
A compiler with these sanitizers and the initialized submodule are required.
See [COV delivery recovery](BACNET_COMMAND_RECOVERY.md#confirmed-cov-delivery-recovery-112-development-candidate)
for behavior and measurement limits. This harness does not replace supervised
hardware commissioning or prove physical switch-to-door timing.

Version 1.0.0 passed complete Windows Chrome and Edge hardware runs with
native USB and file choosers, full verified backups, erase/flash/boot, key
handoff and locking, Ethernet OTA, signed reboot, and final BACnet checks.
The installer pins Espressif's maintained S3 RAM loader after the legacy
loader lost USB bytes during Windows testing. The tested firmware source is
unchanged by that browser-loader replacement. See the exact results and
architecture limits in [platform testing](PLATFORM_TESTING.md#version-10-release-validation).

Version 0.14.0 browser-installer validation on 2026-09-07 used the physical
ESP32-S3-POE-ETH-8DI-8RO-C and Chrome for Testing 153.0.8010.12 on macOS.
The production preview completed a full 16,777,216-byte USB backup with
per-chunk and whole-flash MD5 verification, then erased and installed the
SHA-256-checked release image through the browser. Flash verification,
first-boot USB status, Ethernet discovery, private key download, and explicit
key-export locking passed. Reconnecting detected the running firmware without
another reset and rejected a key request after locking.

The downloaded key loaded through the embedded management page and signed a
successful Ethernet OTA upload of the 587,152-byte application. The controller
booted the other OTA slot; the same key then authorized a reboot. Directed
BACnet discovery and 26 read-only property checks passed after USB installation
and again after OTA. All eight inputs and relay commands were inactive; RTC,
relay-controller, Ethernet, and BACnet health checks passed. Private backups,
keys, and site-specific reports are retained outside the repository.

The Windows hardware run also completed the full browser backup, erase/flash,
USB boot, key download/lock, Ethernet handoff, signed OTA, and signed reboot in
Edge 152.0.4191.66 on Windows 11 ARM64 with real USB passthrough. A Windows
disconnect reset was reproduced and fixed: USB setup now clears RTS before
DTR in separate calls before closing the port. This avoids the intermediate
RTS=1, DTR=0 reset state described in the USB Serial/JTAG chapter of the
[ESP32-S3 technical reference manual](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf).
Separate calls matter because Chromium's Windows
[signal-control implementation](https://chromium.googlesource.com/chromium/src/+/3f506bc44cf5a6c0c7c0058b5f7ab31eead19fbd/services/device/serial/serial_io_handler_win.cc)
processes DTR before RTS when both are supplied together. Hardware reconnect
and disconnect checks then passed without a reset. The expanded 22-test Node
suite, 15 Chromium browser tests, and production build passed; 26 read-only
BACnet checks passed after Windows installation and OTA/reboot.

See [platform testing](PLATFORM_TESTING.md) for exact Windows driver, VM,
automation, and native-dialog coverage limits. This validates the installation
workflow, not the full hardware acceptance or endurance suite below. Physical
BOOT-button key recovery and public GitHub Pages deployment remain separate
checks. Automated tests cover the BOOT hold duration, export expiry, and lock
policy.

The repeatable live BACnet suite and its explicit relay safety gate are
documented in [Hardware acceptance testing](HARDWARE_TESTING.md). Its JSON
report should be retained with release test evidence; do not commit site
addresses, commissioning keys, or unrelated BAS inventory.

Run long-duration health monitoring with the host-side logger described in
[Soak testing](SOAK_TESTING.md). It probes both BACnet/IP and HTTP, detects
restarts/configuration changes/relay activity/heap-floor violations, and does
not add periodic writes to ESP32 NVS or flash.

Version 0.13.5 bench validation on 2026-09-06 completed two full 94-check HIL
runs with BACpypes3 0.0.106. The suite covered directed and broadcast
discovery, the exact 28-object model and metadata, advertised capabilities,
ReadPropertyMultiple `ALL`, Who-Has by identifier and configured name,
confirmed and unconfirmed COV, the 16-subscription capacity boundary, negative
access cases, priority arbitration, reserved priority 6, and final cleanup.
Each Binary Output was the only active relay for three seconds and was
cross-checked against the TCA9554 command mask before relinquishing. A second,
independent upstream bacnet-stack 1.6.0 client also discovered the controller
and read Device, DI, BO, and Network Port properties. Five bursts each at 32
and 64 simultaneous directed Who-Is requests returned all 480 expected I-Am
responses. Intentional overload at 128 and 256 requests plateaued at 64-67
responses without a reboot or state change, consistent with the compiled
64-datagram mailbox reported by the management API.

The same final-candidate validation cycle covered management API/security
headers, precise input-validation errors, correct HMAC, replay, signature,
body-hash, and nonce-expiry behavior; persistent
Device/DI/BO names and location across reboot followed by exact restoration;
DHCP address, Ethernet MAC, OTA partition, and relay safety across repeated
warm reboots; 100 ICMP replies with no host-interface errors or collisions;
relay-state save/restore followed by safe clearing with restore disabled; and
2,011 malformed BACnet/IPv4 frames followed by 100 successful BACnet probes
with bounded heap. Ethernet OTA was validated both by the successful v0.13.5
upgrade and by rejection of missing authentication, an interrupted transfer,
a signed incorrect digest, a structurally invalid image, and a checksum- and
SHA-valid ESP32-S3 image with the wrong project identity. A subsequent reboot
proved that none of the rejected images changed the next boot partition. The
tested OTA image is 578,160 bytes with SHA-256
`8816c0e8ed9684b2c87eec7eff1f88deb4b14a23498080dbb5266179d283d75e`.
The final relay mask and all priority arrays were zero. Host tests, a clean
ESP-IDF 5.5.4 build, package checksums, and GitHub CI also passed.

An earlier v0.13.0 diagnostic soak completed 806 good samples in approximately
13.45 hours and observed one directed I-Am timeout while another BACnet client
was generating an unusually high request rate. The controller did not restart
or lose HTTP service and immediately passed 300 BACnet and 100 ICMP recovery
probes. That evidence led to the receive-mailbox correction above. The
diagnostic run was intentionally stopped after the correction was identified;
it is not a passing endurance result.

The strict v0.13.2 soak subsequently ran the full 86,400 seconds and recorded
1,441 samples: 1,437 complete successes, four request failures, two samples
with health alerts, and a maximum of two consecutive failures. Two isolated
I-Am timeouts recovered at the next one-minute sample. Near the final three
minutes, two more I-Am probes failed and the following status response showed
a new persistent reboot count, 115 seconds of uptime, and ESP-IDF reset reason
`power-on`. The monitoring host did not reboot and its interface counters
remained clean. This is evidence of an external power/reset-path interruption,
not a firmware software-reset reason, but the run correctly failed the strict
zero-failure/zero-reboot gate. Separate burst characterization then showed an
exact 32-response ceiling in v0.13.2 and motivated the supported maximum of 64
plus the compile-time and runtime capacity checks in v0.13.5. A fresh strict
v0.13.5 soak then passed on 2026-09-07 through 2026-09-08 UTC: all 1,441
scheduled samples completed over 86,400 seconds, with zero request failures,
health alerts, or restarts. Current free heap changed by -24 bytes and the
lowest reported historical minimum was 266,344 bytes. See the exact
[completed endurance result](SOAK_TESTING.md#completed-endurance-result).
This result belongs to v0.13.5, not to later firmware versions.

Version 1.1.0 was rechecked on 2026-09-09 UTC using BACpypes3 0.0.106:
56 non-actuating HIL checks passed, with the relay sequence and subscription
capacity stress test explicitly skipped. Existing BAS subscriptions were
left in place. The scan covered all 28 objects and 385 RPM property values,
directed/broadcast discovery, Who-Has, confirmed/unconfirmed COV, health, and
negative access cases. All eight outputs were inactive with clear priority
arrays, and the Device instance remained locked. The three C host executables,
45 Python tests (Python 3.13), 23 Node tests, 22 Chromium interaction tests,
installer production build, and ESP-IDF 5.5.4 application build also passed.
This is a finite regression check, not a v1.1.0 endurance or electrical result.

Read-only interoperability inspection also verified an existing Metasys NAE
9.0.5.7692 mapping to the v1.1.0 controller: all eight Binary Outputs had
confirmed COV subscriptions, and the two inspected mapped outputs matched
the remote objects' inactive Present_Value, clear priority arrays, and normal
health. This verifies retained mappings and live reads/subscriptions, not
end-to-end command generation, command restoration, or physical contacts.
ALC and Niagara/Tridium workstation checks remain outstanding.

The DI channels remained inactive because no electrical stimulus or loopback
fixture was attached. Their BACnet read path and metadata are validated, but
their optocoupler polarity, debounce timing, and field wiring are not.

Remaining field/recovery-access gates:

- electrically stimulate every DI and measure polarity and debounce timing;
- verify every relay contact with a meter, independent of command readback;
- cold boot, cable loss, brownout, watchdog, and rapid power-cycle behavior;
- live static-IP switchover with local USB recovery available;
- failed-startup OTA rollback and corrupt-NVS fallback with recovery access;
- discovery and point import from representative ALC and Niagara/Tridium
  clients, plus Metasys command-source and post-reboot reassertion checks.

A new strict 24-hour run is required to claim endurance validation for a later
firmware version. The completed v0.13.5 result does not remove that version-
specific gate.

## Package

Inside the IDF environment:

```sh
python tools/package_release.py
```

The script reads IDF's `flasher_args.json`, validates the app descriptor,
merges the initial image with esptool, copies the OTA/individual images and
licenses, and creates `manifest.json` plus `SHA256SUMS`. It refuses to overwrite
an existing version directory.

Increase the CMake project version for every distributable firmware change.
The embedded project name must remain stable because the device rejects OTA
images for any other project.

## Release gate

Do not label a release hardware-validated until the physical checklist above
is recorded. Do not publish commissioning keys. Do not enable eFuse security
features without a separate provisioning/recovery design and explicit device
owner approval.
