# Development and release

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

```sh
tests/run_host_tests.sh
idf.py fullclean
idf.py build
idf.py size
git diff --check
```

Host tests cover persistent-model validation/CRC behavior; management-client
URL, key, OTA descriptor, canonical request, HMAC, and relay command behavior;
and static integrity of the embedded management interface. The IDF build
compiles the real ESP32-S3 application, embedded page, and selected
bacnet-stack sources.

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
v0.13.5 soak remains the endurance release gate.

The DI channels remained inactive because no electrical stimulus or loopback
fixture was attached. Their BACnet read path and metadata are validated, but
their optocoupler polarity, debounce timing, and field wiring are not.

Remaining field-only release gates:

- electrically stimulate every DI and measure polarity and debounce timing;
- verify every relay contact with a meter, independent of command readback;
- cold boot, cable loss, brownout, watchdog, and rapid power-cycle behavior;
- live static-IP switchover with local USB recovery available;
- failed-startup OTA rollback and corrupt-NVS fallback with recovery access;
- discovery and point import from representative ALC, Niagara/Tridium, and
  Metasys clients;
- complete the documented strict 24-hour v0.13.5 endurance/soak run.

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
