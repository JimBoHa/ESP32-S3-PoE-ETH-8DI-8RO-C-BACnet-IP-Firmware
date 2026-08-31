# ESP32-S3-PoE-ETH-8DI-8RO-C BACnet/IP firmware

Native ESP-IDF firmware that exposes all eight digital inputs, all eight relay
commands, and device health from the Waveshare
`ESP32-S3-POE-ETH-8DI-8RO-C` as BACnet/IP objects. It replaces the factory
application and provides authenticated firmware updates over Ethernet.

> **Pre-hardware release:** version 0.1.0 builds and passes host tests, but has
> not yet been flashed to or hardware-in-the-loop tested on the target board.
> Disconnect controlled loads during first commissioning.

This source targets only the 16 MB flash / 8 MB PSRAM model built around an
`ESP32-S3-WROOM-1U-N16R8`, W5500 Ethernet controller, and TCA9554 relay
expander. Verify the exact product label and board revision before flashing.

## Features

- BACnet/IP over the onboard W5500, UDP port 47808 by default.
- DHCP by default, with persistent static IPv4 configuration available.
- Eight debounced, active-low-by-default Binary Input objects.
- Eight commandable Binary Output objects with BACnet priority arrays.
- Ethernet, IPv4, relay-controller, RTC, uptime, heap, and reboot status.
- Who-Is, Who-Has, ReadProperty, ReadPropertyMultiple, WriteProperty, and
  SubscribeCOV services.
- NVS-backed configuration with schema, length, and CRC validation.
- Safe relay startup: all relays are explicitly cleared before expander pins
  become outputs. Relay-state restore is available but disabled by default.
- Dual OTA slots, rollback, image validation, project-identity validation, and
  HMAC-SHA256 authenticated Ethernet uploads.
- Read-only status/configuration HTTP endpoints and a standard-library Python
  commissioning client.

## BACnet object map

| Object | Instances | Meaning | BACnet write access |
|---|---:|---|---|
| Device | 599153 default | Identity, services, revision, location | None; configure through authenticated API |
| Binary Input | 1-8 | DI1-DI8 debounced input state | None |
| Binary Output | 1-8 | RO1-RO8 commanded relay state | Present_Value, priorities 1-16 |
| Binary Input | 1001 | Ethernet physical link | None |
| Binary Input | 1002 | IPv4 address assigned | None |
| Binary Input | 1003 | TCA9554 responding | None |
| Binary Input | 1004 | PCF85063 responding | None |
| Analog Input | 1001 | Uptime, seconds | None |
| Analog Input | 1002 | Current free heap, bytes | None |
| Analog Input | 1003 | Minimum free heap, bytes | None |
| Analog Input | 1004 | Persistent reboot count | None |
| Network Port | 1 | BACnet/IPv4 Ethernet port | None |

Relay Present_Value is the **commanded** state. This hardware has no relay
contact-feedback input, so it cannot prove that a contact moved or a load
energized. See [BACnet implementation details](docs/BACNET_PICS.md).

## Build

Prerequisites: Git, Python 3, and ESP-IDF 5.5.4.

```sh
git clone --recurse-submodules https://github.com/JimBoHa/ESP32-S3-PoE-ETH-8DI-8RO-C-BACnet-IP-Firmware.git
cd ESP32-S3-PoE-ETH-8DI-8RO-C-BACnet-IP-Firmware
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

The BACnet dependency is pinned as a submodule to `bacnet-stack-1.6.0`.

Run host tests and package field artifacts:

```sh
tests/run_host_tests.sh
python tools/package_release.py
```

The package is written to `release/v0.1.0/` and contains:

- `initial-flash.bin` for the first USB installation;
- `firmware-ota.bin` for later Ethernet updates;
- individual bootloader, partition-table, and OTA-data images;
- a manifest, SHA-256 checksum list, and license notices.

The application partition is 6 MiB. Version 0.1.0 occupies about 0.51 MiB.

## Persistent configuration

On first boot the firmware installs safe defaults and creates a random 32-byte
admin key in NVS. The key is printed to the USB serial console once. Save it in
a mode-0600 file; it cannot be retrieved through the network API.

Public read-only calls:

```sh
python tools/device_admin.py --device 192.168.75.153 status
python tools/device_admin.py --device 192.168.75.153 config-get
```

Configuration updates are partial JSON objects. Values are validated, written
to NVS, assigned a new database revision, and applied after reboot:

```json
{
  "device_instance": 599153,
  "device_name": "BACnet IO 599153",
  "location": "Mechanical room",
  "dhcp_enabled": false,
  "ip_address": "192.168.75.153",
  "netmask": "255.255.255.0",
  "gateway": "192.168.75.1",
  "dns_server": "192.168.75.1"
}
```

```sh
python tools/device_admin.py \
  --device 192.168.75.153 \
  --key-file device.key \
  config-set --file site-config.json

python tools/device_admin.py \
  --device 192.168.75.153 \
  --key-file device.key \
  reboot --yes
```

Configurable fields are `device_instance`, `bacnet_port`, `vendor_id`,
`vendor_name`, `input_invert_mask`, `dhcp_enabled`, `restore_relay_state`,
`hostname`, `device_name`, `location`, the four static IPv4 strings, and the
eight-element `input_names` and `relay_names` arrays.
BACnet object names must be nonempty, printable ASCII, and unique
case-insensitively. If either name array is present, it must contain exactly
eight strings.

Default vendor ID 260 belongs to **BACnet Stack at SourceForge**, the protocol
implementation used here. Configure the firmware owner's ASHRAE-assigned
vendor ID and name before distributing this as a product. Do not invent an ID.

## Ethernet OTA

Upload only `firmware-ota.bin`, never the merged initial-flash image:

```sh
python tools/device_admin.py \
  --device 192.168.75.153 \
  --key-file device.key \
  ota --file release/v0.1.0/firmware-ota.bin --yes
```

The client checks the ESP image header and project identity. The device signs
the request context with the commissioning key, verifies the received SHA-256,
asks ESP-IDF to verify the image, checks the embedded project identity, writes
only the inactive slot, and reboots. The new image confirms itself only after
NVS, TCA9554, W5500, BACnet task, and management server initialization succeed;
otherwise the bootloader rolls back.

Management uses HTTP, not TLS. HMAC protects command authenticity and request
body integrity, but traffic and responses are not confidential. BACnet/IP is
also unauthenticated. Put the device on a controlled automation VLAN and apply
ACLs. Read [Security](docs/SECURITY.md) before deployment.

## Documentation

- [Commissioning and recovery](docs/COMMISSIONING.md)
- [Hardware mapping and electrical cautions](docs/HARDWARE.md)
- [BACnet implementation/PICS summary](docs/BACNET_PICS.md)
- [Security model](docs/SECURITY.md)
- [Development and release process](docs/DEVELOPMENT.md)

## Current limitations

- Not yet tested on physical target hardware and not BTL tested or listed.
- Local-subnet BACnet/IP only; no BBMD registration or BACnet/SC.
- No CAN, TF-card, buzzer, RGB LED, or RTC timekeeping objects yet.
- IPv4 only; no IPv6.
- No TLS, Secure Boot, flash encryption, or eFuse provisioning.
- Loss of the admin key requires USB recovery or an NVS erase.

## License

Project-owned code is Apache-2.0. The pinned bacnet-stack dependency uses
per-file licenses, primarily GPL-2.0-or-later with GCC-exception-2.0, plus MIT
and Apache-2.0 files. See [third-party notices](THIRD_PARTY_NOTICES.md) and the
license directory inside the submodule.
