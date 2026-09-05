# BACnet implementation summary

This is an engineering PICS-style summary for firmware 0.12.0. It is not a BTL
test report, listing, or certification claim.

## Device and data link

| Item | Implementation |
|---|---|
| Data link | BACnet/IPv4 (Annex J style BVLL) over W5500 Ethernet |
| Default UDP port | 47808 (`0xBAC0`), persistent configurable |
| Default Device instance | 599153, persistent configurable |
| Protocol revision | 28 |
| Maximum APDU | 1476 octets |
| Segmentation | Not supported |
| BBMD / Foreign Device | Not implemented |
| BACnet/SC | Not implemented |
| Character set | ANSI X3.4 strings emitted by the stack |
| COV subscriptions | Up to 16 subscriptions / 8 addresses |

Broadcast discovery works on the local IP subnet. Cross-subnet discovery needs
an external BACnet router/BBMD design; the device does not register as a Foreign
Device.

## Services executed as server

- Who-Is with I-Am response
- Who-Has with I-Have response
- ReadProperty
- ReadPropertyMultiple
- WriteProperty
- SubscribeCOV, with confirmed or unconfirmed notifications as requested

The firmware also sends I-Am after BACnet/IP starts. It does not implement
DeviceCommunicationControl, ReinitializeDevice, WritePropertyMultiple, time
synchronization, alarm/event services, file transfer, or private transfer.

Broadcast Who-Is requests receive a broadcast I-Am. Unicast Who-Is requests
receive a unicast I-Am directed back to the requesting BACnet/IP address. A
Forwarded-NPDU is treated as broadcast traffic. This supports both ordinary
same-subnet discovery and directed discovery through networks that suppress
broadcast replies.

## Discovery and configured metadata

The standard I-Am payload does not contain a device name or point names. It
contains only the Device object identifier, maximum accepted APDU length,
segmentation support, and vendor identifier. After I-Am, a BACnet browser can
read the following standard properties to complete discovery:

| Configuration | BACnet representation |
|---|---|
| Device instance | I-Am identifier and Device Object_Identifier |
| Device name | Device Object_Name |
| Vendor name and ID | Device Vendor_Name and Vendor_Identifier |
| Location | Device Location |
| Firmware/model/serial | Device identity properties |
| Database revision | Device Database_Revision |
| DI and RO names | BI/BO Object_Name; objects enumerated through Object_List |
| DI electrical inversion | BI Polarity and Description |
| DHCP, active IP, subnet, gateway, DNS, UDP port | Network Port 1 |
| Ethernet hostname | CharacterString Value 1 |
| Relay-state restore policy | Binary Value 1 |

ReadProperty and ReadPropertyMultiple support this object scan. Who-Has also
accepts an object name and produces I-Have. A discovery tool that displays only
the initial I-Am fields must perform the property scan before names appear.
Inactive fallback static-address fields are management configuration rather
than active network state and are not duplicated into BACnet while DHCP is in
use.

## Objects

### Device, configured instance

Identity, model, firmware/application versions, serial derived from Ethernet
MAC, location, restart information, supported object/service bit strings,
object list, database revision, and active COV subscriptions are exposed.
Properties are read-only over BACnet. Persistent identity changes use the
authenticated management API and take effect after reboot.

Vendor ID/name default to bacnet-stack's assigned vendor identity, ID 260.
Configure an owner-assigned ASHRAE vendor identity before product distribution.

### Binary Input 1-8

Physical DI1-DI8. Present_Value changes after a 60 ms nominal debounce period.
Object_Name is the persistent configured input name. Polarity reports
`REVERSE` for a configured active-low channel and `NORMAL` for active-high.
Description also states the configured electrical polarity. Reliability is
`NO_FAULT_DETECTED`. These objects are read-only and support COV.

### Binary Output 1-8

Relay commands RO1-RO8. Present_Value implements the standard 16-level priority
array and relinquish default. Only Present_Value is writable; object metadata,
Out_Of_Service, polarity, and other properties are read-only. These objects
support COV.

The firmware callback applies effective Present_Value to the TCA9554. If the
I2C write fails, all relay objects report `UNRELIABLE_OTHER`. No auxiliary
contact exists, so the value is not physical feedback.

Authenticated HTTP and web-interface relay commands update the same priority
array and invoke the same physical-output callback as BACnet WriteProperty;
they do not maintain a parallel or hidden relay state.

### Status Binary Inputs 1001-1004

| Instance | Object name | Active means |
|---:|---|---|
| 1001 | Status Ethernet Link | W5500 reports physical link up |
| 1002 | Status IPv4 Assigned | Interface owns an IPv4 address |
| 1003 | Status Relay Controller | TCA9554 responds to periodic I2C writes |
| 1004 | Status RTC Present | PCF85063 responds to an I2C probe |

All are read-only and support COV.

### Status Analog Inputs 1001-1004

| Instance | Object name | Units / update |
|---:|---|---|
| 1001 | Status Uptime Seconds | seconds; COV increment 60 |
| 1002 | Status Free Heap Bytes | no-units; COV increment 1024 |
| 1003 | Status Minimum Heap Bytes | no-units; COV increment 1024 |
| 1004 | Status Reboot Count | no-units; COV increment 1 |

All are read-only and support COV.

### Network Port 1

Read-only BACnet/IPv4 port information: UDP port, DHCP flag, IP address,
gateway, prefix, active DNS server, BACnet/IP MAC, APDU length, mode, link
speed, quality, reliability, and Out_Of_Service state.

### Binary Value 1

`Configuration Relay State Restore` is a read-only representation of the
persistent power-up policy. Present_Value is Active/`Enabled` only when relay
state restoration is configured; default is Inactive/`Disabled`.

### CharacterString Value 1

`Configuration Hostname` is read-only and contains the persistent Ethernet
hostname applied at boot.

## Persistence

Configuration changes are stored in NVS with schema, size, and CRC checks.
Output commands and priority arrays are not normally retained across a power
cycle: safe default is all relays off. If `restore_relay_state` is explicitly
enabled, the effective physical mask is saved after five stable seconds and
used as the next boot's relinquish default.

Enable relay restore only when the process has been reviewed for automatic
re-energization after an outage. It also increases NVS writes when relay states
remain changed for at least five seconds.
