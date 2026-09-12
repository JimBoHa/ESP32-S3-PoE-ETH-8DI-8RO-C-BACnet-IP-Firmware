# Relay output recovery — 1.1.2 development candidate

This candidate repairs reproduced output-driver failure mechanisms. It was
installed on one output controller on September 10, 2026 and passed bounded
BACnet command/register-readback tests. Those tests do not establish the cause
of the earlier field incident or physical switch-to-door timing. It is
separate from BACnet COV delivery recovery, which repairs a notification-delivery
path rather than relay application.

## Correction

Previously, a 250 ms relay-mutex timeout returned before recording the desired
command. BACnet had already accepted its effective priority value through a
void output callback, but the periodic driver retry retained the old mask.
Desired state is now published under a short critical section before waiting
for the hardware mutex. After obtaining the mutex, the driver reads the latest
desired mask. Older commands are not queued or replayed; both On and Off use
this path.

Previously, only initialization configured TCA9554 pin direction; periodic
checks merely wrote the output latch. An independent expander reset returns
pins to inputs. Latch writes can then acknowledge without driving those pins.
Application and periodic checks now read CONFIG and OUTPUT. Direction recovery
requires a written and verified desired latch before enabling pins, followed
by another verification. Errors leave the latest desired command pending.

The [TI TCA9554 datasheet](https://www.ti.com/lit/ds/symlink/tca9554.pdf),
sections 8.3.1 and 8.6.3, documents these reset defaults and register behavior.
The [ESP-IDF 5.5.4 I2C API](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/peripherals/i2c.html)
provides register reads. These establish mechanisms, not evidence that a reset
or mutex timeout actually occurred at the installation.

## Timing and ownership

Normal commands still apply synchronously. Recovery uses the existing nominal
one-second health cadence and 100 ms per-operation I2C timeout. Scheduling,
bus locking, and multiple transactions prevent a hard real-time guarantee.
Routine verification does not report a false fault merely while in flight.

Boot still defaults Off. This change does not enable relay-state restoration,
retain BACnet priorities across reboot, clear another writer, or manufacture
a missing Metasys command. Existing priority arbitration is unchanged.

## Diagnostics

`GET /api/v1/status` adds an internally consistent `relay_driver` snapshot:

| Field | Meaning |
| --- | --- |
| `desired_mask` | Latest effective command delivered to the driver |
| `applied_mask` | Last mask verified against CONFIG/OUTPUT |
| `healthy` | Successful verification and desired equals applied |
| `registers_valid` | Complete readable CONFIG/OUTPUT snapshot exists |
| `output_register`, `configuration_register` | Last read registers; null when invalid |
| `i2c_errors`, `verification_failures`, `mutex_timeouts` | Per-boot failure counters |
| `configuration_recoveries` | Completed, verified direction repairs |
| `last_error` | Last error; cleared by successful verification |
| `last_verified_ms` | Device uptime at the last successful verification |

A readable mismatch may have `registers_valid=true` but `healthy=false`.
After failure, `applied_mask` is historical, not current hardware proof. Legacy
`relay_outputs_mask` now means the last verified mask. Neither these registers
nor a BACnet ACK provide relay-contact or door-position feedback.

## Tests and deployment

`python3 tests/run_board_io_tests.py` compiles the actual driver with deterministic
FreeRTOS/GPIO/I2C stubs and AddressSanitizer/UndefinedBehaviorSanitizer. It
models failed locking, failed transactions, expander resets, and acknowledged
but unchanged latch writes. No socket or hardware is used. The full host suite
invokes it. With a preserved pre-fix driver, `--source /private/path/board_io.c
--expect-baseline` asserts the old failures; passing that mode means reproduced
failure, not correctness. Baseline copies are not tracked.

Before deployment, retain the tested image identity, current configuration,
priority ownership, and rollback image. OTA clears volatile priority state;
Metasys reassertion must be verified separately. Connected-load tests must use
current operator authority, bounded actuation, and cleanup. Do not run the full
disconnected-load HIL suite on live doors.
