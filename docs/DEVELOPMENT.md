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
`partitions.csv` define the reproducible inputs.

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

Version 0.13.2 bench validation on 2026-09-05 completed two full 93-check HIL
runs with BACpypes3 0.0.106. The suite covered directed and broadcast
discovery, the exact 28-object model and metadata, advertised capabilities,
ReadPropertyMultiple `ALL`, Who-Has by identifier and configured name,
confirmed and unconfirmed COV, the 16-subscription capacity boundary, negative
access cases, priority arbitration, reserved priority 6, and final cleanup.
Each Binary Output was the only active relay for three seconds and was
cross-checked against the TCA9554 command mask before relinquishing. A second,
independent upstream bacnet-stack 1.6.0 client also discovered the controller
and read Device, DI, BO, and Network Port properties. Ten bursts of 32
simultaneous directed Who-Is requests returned 320 of 320 I-Am responses after
the BACnet UDP receive mailbox was increased from 6 to 32 datagrams.

The same final-candidate validation cycle covered management API/security
headers, precise input-validation errors, correct
HMAC, replay, signature, body-hash, and nonce-expiry behavior; persistent
Device/DI/BO names and location across reboot followed by exact restoration;
DHCP address and MAC stability across repeated warm reboots; 100 ICMP replies
with no host-interface errors, drops, or collisions; safe relay clearing when
rebooting with restore disabled; and 2,011 malformed BACnet/IPv4 frames
followed by 100 successful BACnet probes with bounded heap. Ethernet OTA was
validated both by a successful upgrade and by rejection of an interrupted
transfer, a signed incorrect digest, a structurally invalid image, and a
checksum/hash-valid image with the wrong project identity. A subsequent reboot
proved that none of the rejected images changed the next boot partition. The
tested OTA image is 577,840 bytes with SHA-256
`fab8d1b3308829c60cadfc3b60242d8d5bd76cdb411104d00a9106780631f973`.
The final relay mask and all priority arrays were zero.

An earlier v0.13.0 diagnostic soak completed 806 good samples in approximately
13.45 hours and observed one directed I-Am timeout while another BACnet client
was generating an unusually high request rate. The controller did not restart
or lose HTTP service and immediately passed 300 BACnet and 100 ICMP recovery
probes. That evidence led to the receive-mailbox correction above. The
diagnostic run was intentionally stopped after the correction was identified;
it is not a passing endurance result. A fresh strict v0.13.2 soak remains a
release gate.

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
- complete the documented strict 24-hour v0.13.2 endurance/soak run.

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
