# Network time — 1.1.4 development candidate

The controller uses ESP-IDF 5.5.4's SNTP client to obtain UTC from an NTP
server. The default is `pool.ntp.org`; the default display timezone is `UTC0`.
The management page's Configuration tab contains a separate Network time form
with common timezone presets and a custom POSIX timezone rule. For example,
America/Los_Angeles uses `PST8PDT,M3.2.0/2,M11.1.0/2`, including daylight saving.
The NTP server can be a DNS hostname or unicast IPv4 address, without a URL,
port, credentials, or path. DNS and outbound UDP port 123 must be available.

Time settings are independent of device identity, network configuration,
BACnet point names, restart recipients, priorities, and relay startup policy.
They are stored in a separate versioned `bacnet_cfg/time_config` NVS blob.
The original `firmware_config_t` layout and its CRC are unchanged. Settings
are validated and committed before becoming visible; identical saved settings
avoid another flash write. Missing or corrupt settings use the safe defaults.

## Runtime behavior

- Time configuration changes do not require rebooting. A dedicated owner task
  applies them asynchronously; the save response reports `apply_pending`.
- The owner resolves DNS without holding BACnet, relay, or timezone locks.
  It discards results if the configuration or network changed during lookup.
  The pinned SNTP client receives only binary IP addresses, never asynchronous
  hostname lookups. This avoids a DNS callback arriving after SNTP has stopped.
- Initial SNTP requests have a random delay below 500 ms. Clock/DNS refresh is
  approximately hourly. DNS/start failures retry after 30 seconds. Individual
  NTP packet retries remain governed by the pinned client, not a tight loop.
- Clock synchronization does not block BACnet requests or relay processing.
  The restart procedure waits at most five seconds after application/BACnet
  readiness for a valid clock; I-Am does not wait for NTP. See
  [restart notifications](BACNET_RESTART_NOTIFICATIONS.md).
- A valid NTP response is anchored to monotonic uptime. The estimated boot UTC
  is that UTC sample minus uptime at the sample, not the later synchronization
  instant. BACnet freezes its selected boot timestamp once. Subsequent NTP
  corrections and timezone changes do not manufacture another restart event
  or alter that boot's already-selected notification timestamp.
- A normal software restart or deep-sleep wake can reuse the advancing ESP-IDF
  RTC-backed POSIX clock, but only with a valid versioned/CRC-checked retained
  NTP marker and an age between zero and seven days. The saved last-sync epoch
  is only a validation marker; it is not replayed as the current time. This is
  reported as `retained-ntp`, not a fresh synchronization. Power-on, brownout,
  unsupported reset causes, invalid RAM, and implausible time reject holdover.
- No RTC or NVS write occurs on each clock tick. NTP synchronization updates
  retained RTC RAM, not flash. The external I2C RTC is not used as a clock source.
- If no usable clock is available, the advancing BACnet clock and the frozen
  restart timestamp use the explicit unsynchronized 1990 fallback. A late NTP
  response updates current time but does not rewrite that frozen boot stamp.

BACnet Local_Date and Local_Time report the selected timezone. UTC_Offset
reports standard-time minutes west of UTC; Daylight_Savings_Status is separate.
For Los Angeles, UTC_Offset is +480 in both winter and summer, while the DST
flag changes. Custom standard/DST offsets have minute precision; explicit
transition rules are required. IANA zone paths are not a newlib timezone
database and must not be entered as custom rules.

## API and diagnostics

`GET /api/v1/time` reads the server, timezone and clock status.
`PUT /api/v1/time` requires the existing HMAC authentication and exactly:

```json
{"ntp_server":"pool.ntp.org","timezone":"PST8PDT,M3.2.0/2,M11.1.0/2"}
```

`GET /api/v1/status` also includes a `time` object: validity, whether an NTP
response was accepted this boot, source, running state, accepted/rejected sync
counts, DNS/start failures, configuration generation, last-sync time and age,
and formatted UTC/local time. The configured server is not a cryptographic
assertion about the source of a previous response. `synchronized` records
accepted NTP this boot; inspect the age rather than treating it as perpetual
proof of current server reachability. Restart and I-Am diagnostics separately
report timestamp selection and transport attempts; UDP acceptance is not proof
of supervisory receipt, processing, subscription renewal or command replay.

## Validation and security limits

The pinned lwIP target is built with `SNTP_CHECK_RESPONSE=2` to verify reply
source/port and the echoed request timestamp. The documented ESP-IDF weak
`sntp_sync_time` hook validates microseconds and a supported 2020–2099 UTC epoch
before updating POSIX time. Rejected samples preserve the previous clock
anchor and do not become a trusted restart timestamp. This is ordinary NTP,
not authenticated Network Time Security (NTS); it does not authenticate a
public server or defeat an on-path attacker. Use a trusted local NTP server
where the deployment requires that trust boundary. No time-service change
grants BACnet writes to Device time properties or changes output commands.

Offline host tests exercise the actual service with intercepted POSIX writes
and deterministic IDF/network stubs, plus arithmetic, validation, persistence,
restart scheduling and timezone tests. The embedded page has browser tests
against a mock device. These tests never set the host clock or operate relays.
They do not replace field NTP, warm-reset, web-save, BACnet property and relay
driver checks, and do not constitute physical-contact or BAS-recovery proof.

Primary implementation references:

- [ESP-IDF 5.5.4 system time and SNTP](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/system_time.html)
- [ESP-NETIF SNTP service](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/network/esp_netif_programming.html#sntp-service)
- [BACnet UTC offset clarification](https://bacnet.org/wp-content/uploads/sites/4/2022/08/Add-135-2012bg.pdf)

For distribution beyond this installation, review the NTP Pool's
[vendor guidance](https://www.ntppool.org/en/vendors.html) before shipping a
large fleet using the public pool default.
