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

Initial 0.10.0 bench validation on 2026-08-31 read each Binary Input over
BACnet/IP for at least three seconds and commanded each Binary Output as the
only active channel for at least three seconds. Every BACnet write was read
back and cross-checked against the TCA9554 command mask, then turned off and
relinquished. The DI channels remained inactive because no electrical stimulus
or loopback fixture was attached; this validates their BACnet read path, not
their optocoupler polarity or field wiring. The final relay mask and all
priority arrays were zero.

Tests still required on physical hardware:

- cold boot, brownout, watchdog, and rapid power-cycle relay behavior;
- all DI polarities and debounce timing;
- every relay mapping under no-load conditions;
- W5500 link/DHCP/static-IP behavior and MAC stability;
- Who-Is/I-Am, object reads, RPM, priority-array writes, relinquish, and COV
  against at least two independent BACnet clients;
- OTA success, interrupted transfer, invalid hash/image/project, failed
  startup, and bootloader rollback;
- NVS persistence and corrupt-blob fallback;
- endurance/soak, malformed-packet fuzzing, and heap monitoring.

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
