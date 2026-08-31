# Security model

## Network placement

BACnet/IP has no authentication in this firmware. Any host that can reach the
BACnet UDP port can attempt to command relay Present_Value. Put the controller
on a dedicated building-automation VLAN and restrict UDP 47808 (or the chosen
port) to required BACnet peers.

Restrict TCP 80 to commissioning/management hosts. Do not expose either port to
the public Internet. This firmware does not implement BACnet/SC.

## Management authentication

First boot generates a random 256-bit admin key, stores it in NVS, and prints it
once to the USB console. Mutation requests use a 60-second, one-use random nonce
and HMAC-SHA256 over:

```text
BACNET-IO-AUTH-V1
METHOD
PATH
NONCE
CONTENT_LENGTH
BODY_SHA256
```

The firmware verifies hexadecimal formatting, nonce lifetime, content hash,
and HMAC with a constant-time comparison. A nonce is consumed only after a
correct signature. Four challenges can be outstanding.

This protocol runs over plain HTTP. It authenticates the command and request
body but does **not** hide configuration, firmware bytes, addresses, or timing.
Responses are not authenticated, and an active network attacker can delay an
otherwise valid signed command. Network isolation remains mandatory.

Store the key in a dedicated file with mode 0600. Do not pass it directly on a
command line, place it in source control, send it in tickets/chat, or include it
in a release package. The supplied client accepts `--key-file` or the
`BACNET_IO_ADMIN_KEY_FILE` path environment variable.

The API intentionally has no network key-recovery or key-rotation endpoint:
plain HTTP cannot confidentially transport a replacement key. If the key is
lost or suspected compromised, perform a controlled USB reflash/flash erase
and recommission the device.

## OTA controls

An Ethernet update must pass all of these checks:

1. valid HMAC for method, path, nonce, length, and claimed SHA-256;
2. actual streamed SHA-256 equals the signed value;
3. image fits the inactive 6 MiB OTA partition;
4. ESP-IDF image validation succeeds;
5. embedded project name matches the running firmware;
6. boot partition update succeeds.

The new slot is pending until NVS, relay expander, W5500 driver, BACnet task,
and management server initialize. Failure invokes ESP-IDF rollback. Link and
DHCP are not self-test requirements because a cable or DHCP server may be
temporarily absent.

## Deliberately not enabled

- TLS server certificates and HTTPS
- Secure Boot
- flash encryption
- eFuse anti-rollback
- signed public-key firmware verification beyond the authenticated HMAC upload

Secure Boot, flash encryption, and eFuse operations are device-lifecycle
decisions that can be irreversible. They require a separate provisioning,
backup, recovery, and manufacturing-key design; this project does not silently
enable them.

Anyone with physical USB/flash access can replace firmware or extract
unencrypted NVS. The current controls address accidental uploads and network
attackers on a properly restricted LAN, not hostile physical possession.

## Operational safety

Relay commands can energize real equipment. Use electrical interlocks,
overcurrent protection, safe failure modes, and independent life-safety
controls. Test first with controlled loads disconnected. Do not treat software
authentication or BACnet priorities as a safety-rated interlock.
