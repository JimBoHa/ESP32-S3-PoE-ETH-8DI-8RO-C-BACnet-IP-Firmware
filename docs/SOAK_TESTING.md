# Soak testing

`tools/soak_monitor.py` records controller health to an append-only JSONL file.
Each interval it sends a directed BACnet Who-Is and validates the I-Am identity,
then reads the public HTTP status and configuration endpoints. It does not need
the admin key and never writes to the controller.

The monitor treats these conditions as failures:

- a missed or malformed BACnet or HTTP response;
- an unexpected reboot, firmware/partition/IP/identity change, or configuration
  change;
- on v1.1.0 and newer, an unlocked/conflicting instance, missing or invalid
  assignment telemetry, or a change in the cumulative instance-conflict count;
- a missing, undersized, or changing compiled BACnet UDP receive mailbox;
- Ethernet, IPv4, BACnet, TCA9554, or RTC becoming unhealthy;
- unexpected relay command/output state or active priorities;
- current or historical free heap below the configured floor;
- decreasing uptime or BACnet packet counters.

Digital inputs are logged but may change without failing the soak. Host-side
JSONL avoids periodic writes to the ESP32 flash and preserves the exact status,
latencies, alerts, reset cause, configuration fingerprint, and a final summary.

For v1.1.0+, both manual and automatically assigned instances must report
`locked` with `device_instance_locked=true` before a baseline is accepted.
Earlier firmware without assignment telemetry remains supported. A nonzero
conflict count at baseline is allowed when the instance is already locked:
automatic discovery can resolve collisions before the run begins. Any later
count change or `locked-conflict` warning fails the soak even if the numeric
Device instance does not change. These checks use the controller's observed
conflicts; they cannot find an offline or unreachable duplicate device.

For a 24-hour, one-minute-interval run with all relays expected off:

```sh
python tools/soak_monitor.py \
  --device-address 192.168.75.154 \
  --device-instance 599153 \
  --duration 86400 \
  --interval 60 \
  --timeout 5 \
  --minimum-heap-bytes 250000 \
  --expected-relay-mask 0 \
  --summary-every 15 \
  --output ../bacnet-io-soak-24h.jsonl
```

The output path must not already exist. A row is flushed after every sample and
synced every ten rows; a final summary is also synced before close. Samples are
taken at the start and at the exact duration boundary, as well as at every
interval between them. The process returns zero only if the full duration
finishes with no request failures and no health alerts. An interrupted run
writes a summary and returns 130.

For a release gate, require all 1,441 scheduled samples, zero request failures,
zero health alerts, no unexpected restart or relay activity, and a final
summary with `"success":true`. Choose a heap threshold only after measuring the
release's normal baseline; 250,000 bytes provides a modest margin below the
264,508-byte minimum observed during v0.13.5 malformed-packet bench testing.

Inspect progress from another terminal without changing the log:

```sh
tail -n 2 ../bacnet-io-soak-24h.jsonl
```

Do not commit site-specific soak logs. They include public configuration and
network metadata, though never the commissioning key.

## Completed endurance result

The strict v0.13.5 bench run passed from **2026-09-07 04:25:37.709 UTC** to
**2026-09-08 04:25:38.168 UTC**. Its final JSONL summary reported:

| Measurement | Result |
|---|---:|
| Elapsed monitoring time | 86,400.035 seconds |
| Scheduled / successful samples | 1,441 / 1,441 |
| Request failures / samples with health alerts | 0 / 0 |
| Availability | 100% |
| Unexpected restarts | 0 |
| Free heap, first / last | 273,008 / 272,984 bytes |
| Lowest current / historical minimum heap | 272,208 / 266,344 bytes |
| BACnet latency, mean / p95 / maximum | 3.488 / 4.580 / 5.863 ms |
| HTTP status / config maximum latency | 17.570 / 22.647 ms |
| Final `success` / `interrupted` | `true` / `false` |

Relays were expected off, the heap floor was 250,000 bytes, and no firmware,
partition, configuration, or identity changes were permitted. Raw site-specific
evidence is retained outside the repository. This is a passing **v0.13.5**
endurance result; it does not certify later firmware, loaded relay contacts,
electrically stimulated inputs, or power-fault recovery.

Before monitoring a commissioned controller, agree on the expected relay
state and a window without planned reboots or configuration changes. An
intentional BAS command can fail an all-off soak without indicating a firmware
fault; preserve that evidence and explain the event rather than removing the
alert or claiming an uninterrupted pass.
