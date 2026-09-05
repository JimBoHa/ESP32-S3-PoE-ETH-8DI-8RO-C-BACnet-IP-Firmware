# Soak testing

`tools/soak_monitor.py` records controller health to an append-only JSONL file.
Each interval it sends a directed BACnet Who-Is and validates the I-Am identity,
then reads the public HTTP status and configuration endpoints. It does not need
the admin key and never writes to the controller.

The monitor treats these conditions as failures:

- a missed or malformed BACnet or HTTP response;
- an unexpected reboot, firmware/partition/IP/identity change, or configuration
  change;
- Ethernet, IPv4, BACnet, TCA9554, or RTC becoming unhealthy;
- unexpected relay command/output state or active priorities;
- current or historical free heap below the configured floor;
- decreasing uptime or BACnet packet counters.

Digital inputs are logged but may change without failing the soak. Host-side
JSONL avoids periodic writes to the ESP32 flash and preserves the exact status,
latencies, alerts, reset cause, configuration fingerprint, and a final summary.

For a 24-hour, one-minute-interval run with all relays expected off:

```sh
python tools/soak_monitor.py \
  --device-address 192.168.75.154 \
  --device-instance 599153 \
  --duration 86400 \
  --interval 60 \
  --timeout 5 \
  --minimum-heap-bytes 200000 \
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

Inspect progress from another terminal without changing the log:

```sh
tail -n 2 ../bacnet-io-soak-24h.jsonl
```

Do not commit site-specific soak logs. They include public configuration and
network metadata, though never the commissioning key.
