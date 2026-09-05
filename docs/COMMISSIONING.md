# Commissioning and recovery

Version 0.13.2 has completed the software-visible target-board acceptance
suite but not the field electrical, destructive recovery, proprietary-client,
or endurance checks listed in [Development and release](DEVELOPMENT.md). Use a
bench unit, disconnect relay loads, and preserve a way to restore the
manufacturer's image if that matters to the site.

## 1. Verify the target

Confirm the enclosure/PCB is exactly `ESP32-S3-POE-ETH-8DI-8RO-C` with an
ESP32-S3-WROOM-1U-N16R8. Do not use the 8DO model or another pin-compatible-
looking variant. Inspect [the hardware map](HARDWARE.md).

## 2. Verify the release

On the field machine:

```sh
sha256sum -c SHA256SUMS
```

On macOS, use `shasum -a 256 -c SHA256SUMS`. Do not flash if verification
fails. `initial-flash.bin` is for USB installation; `firmware-ota.bin` is only
for updates from a running copy of this firmware.

## 3. Initial USB flash

Install a current esptool release, connect USB-C, and identify the serial port.
If automatic download mode does not engage, hold BOOT, tap RESET, then release
BOOT. Flash erase removes the factory application and all local settings.

```sh
python -m esptool --chip esp32s3 --port /dev/DEVICE erase-flash
python -m esptool --chip esp32s3 --port /dev/DEVICE \
  --baud 460800 write-flash 0x0 initial-flash.bin
```

Never guess the serial path. On Windows it will normally be a `COM` port.

## 4. Capture first-boot information

Open the USB serial console at 115200 baud before or immediately after reset.
The firmware prints:

- derived Ethernet MAC;
- DHCP/static IPv4 address when acquired;
- the random admin key on first boot only.

Save the 64 hexadecimal key characters in a file outside the repository:

```sh
chmod 600 device.key
```

If the key scrolls past before capture, erasing NVS/flash and recommissioning is
the recovery path.

## 5. Find and inspect the device

Default networking is DHCP. Find the derived MAC/IP in serial logs or the DHCP
server lease table. The old factory address is not guaranteed to survive
because firmware and DHCP behavior may differ.

```sh
python tools/device_admin.py --device DEVICE_IP status
python tools/device_admin.py --device DEVICE_IP config-get
```

Check product, firmware version, partition, Ethernet link, IPv4 state,
TCA9554 health, RTC presence, input mask, and relay mask. Relays should be zero
with default settings.

Open `http://DEVICE_IP/` to use the embedded management interface. Live status
does not require a key. Enter the saved admin key only when commanding a relay,
saving configuration, uploading OTA firmware, or rebooting. The page does not
store the key, but it is served over HTTP; use it only across a trusted
automation/management network.

## 6. Apply site configuration

Create a partial JSON file. For a static address on the example subnet:

```json
{
  "device_instance": 599153,
  "hostname": "bacnet-io-599153",
  "device_name": "BACnet IO 599153",
  "location": "Uncommissioned",
  "dhcp_enabled": false,
  "ip_address": "192.168.75.153",
  "netmask": "255.255.255.0",
  "gateway": "192.168.75.1",
  "dns_server": "192.168.75.1",
  "restore_relay_state": false
}
```

Verify that the address is reserved and unused before assignment. Save and
reboot:

```sh
python tools/device_admin.py --device DEVICE_IP --key-file device.key \
  config-set --file site-config.json
python tools/device_admin.py --device DEVICE_IP --key-file device.key \
  reboot --yes
```

Reconnect at the new address and read configuration/status again.

## 7. BACnet checkout

1. Send Who-Is and confirm one I-Am with the intended Device instance.
2. Read the Device object, object list, and Network Port 1.
3. Exercise DI1-DI8 and verify Binary Input 1-8 without relay loads attached.
4. Subscribe to COV and verify state changes.
5. Command only one Binary Output at a time, first at a normal BACnet priority.
6. Relinquish the command and verify the priority array/default behavior.
7. Confirm the correct physical relay and status indicators before connecting
   any load.

For a scripted authenticated check outside a BACnet workstation:

```sh
python tools/device_admin.py --device DEVICE_IP --key-file device.key \
  relay-set --channel 1 --state on --priority 8
python tools/device_admin.py --device DEVICE_IP --key-file device.key \
  relay-set --channel 1 --state relinquish --priority 8
```

Duplicate Device instances break discovery and bindings. Assign a site-unique
instance before connecting multiple units.

## 8. Ethernet OTA

Verify the package checksum, then:

```sh
python tools/device_admin.py --device DEVICE_IP --key-file device.key \
  ota --file firmware-ota.bin --yes
```

The connection closes during reboot. Wait for the normal boot interval, query
status, confirm the firmware version/running partition, and repeat a BACnet
read. If the new image fails its core startup checks, the bootloader returns to
the prior slot.

## Recovery

- **No IP:** inspect serial logs, DHCP leases, link LEDs, PoE/switch VLAN, and
  static configuration. USB reflash remains available.
- **Lost admin key:** USB erase/reflash and recommission. There is no backdoor.
- **OTA rejected:** use the app-only image from the release, verify SHA-256,
  project/version, key file, and partition-size limit.
- **Relay expander failure:** firmware reports fault and a pending OTA image
  rolls back. De-energize loads before hardware diagnosis.
- **Repeated crash:** collect serial output and the flash core dump before an
  erase if forensic data matters.
