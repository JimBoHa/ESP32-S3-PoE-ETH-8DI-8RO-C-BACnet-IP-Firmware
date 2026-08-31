# BACnet implementation summary

This is an engineering PICS-style summary for firmware 0.9.0. It is not a BTL
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
Reliability is `NO_FAULT_DETECTED`. These objects are read-only and support COV.

### Binary Output 1-8

Relay commands RO1-RO8. Present_Value implements the standard 16-level priority
array and relinquish default. Only Present_Value is writable; object metadata,
Out_Of_Service, polarity, and other properties are read-only. These objects
support COV.

The firmware callback applies effective Present_Value to the TCA9554. If the
I2C write fails, all relay objects report `UNRELIABLE_OTHER`. No auxiliary
contact exists, so the value is not physical feedback.

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
gateway, prefix, BACnet/IP MAC, APDU length, mode, link speed, quality,
reliability, and Out_Of_Service state.

## Persistence

Configuration changes are stored in NVS with schema, size, and CRC checks.
Output commands and priority arrays are not normally retained across a power
cycle: safe default is all relays off. If `restore_relay_state` is explicitly
enabled, the effective physical mask is saved after five stable seconds and
used as the next boot's relinquish default.

Enable relay restore only when the process has been reviewed for automatic
re-energization after an outage. It also increases NVS writes when relay states
remain changed for at least five seconds.
