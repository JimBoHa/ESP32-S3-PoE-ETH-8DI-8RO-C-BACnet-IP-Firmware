# Hardware target

Target: Waveshare `ESP32-S3-POE-ETH-8DI-8RO-C`, the PoE and CAN variant with
eight digital inputs and eight relay outputs. The `-C` variant substitutes CAN
for the RS485 interface found on the non-C model. This firmware does not use
CAN.

Primary references:

- [Waveshare product selection page](https://www.waveshare.com/product/modules/esp32-s3-eth-8di-8ro-c.htm)
- [Waveshare ESP32-S3-ETH-8DI-8RO-C wiki](https://www.waveshare.com/wiki/ESP32-S3-ETH-8DI-8RO-C)
- [Waveshare family wiki and interface tables](https://www.waveshare.com/wiki/ESP32-S3-ETH-8DI-8RO)
- [Espressif ESP32-S3 documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/)

## Mapping used by the firmware

| Function | Connection |
|---|---|
| MCU module | ESP32-S3-WROOM-1U-N16R8; 16 MB flash, 8 MB PSRAM |
| DI1-DI8 | GPIO4-GPIO11, input pull-ups, active-low default |
| Relay expander | TCA9554, I2C address `0x20` |
| Relay channels | TCA9554 P0-P7, active-high command |
| I2C | SDA GPIO42, SCL GPIO41, 100 kHz |
| RTC | PCF85063, I2C address `0x51` |
| W5500 IRQ | GPIO12 |
| W5500 MOSI | GPIO13 |
| W5500 MISO | GPIO14 |
| W5500 SCLK | GPIO15 |
| W5500 CS | GPIO16 |
| W5500 reset | GPIO39 |
| CAN | TX GPIO2, RX GPIO3; unused |
| RGB LED | GPIO38; unused |
| Buzzer | GPIO46; unused |

The W5500 has no programmed MAC address. Firmware derives one stable locally
administered MAC from the ESP32's unique Ethernet base MAC, following
Espressif's SPI-Ethernet convention.

## Relay startup sequence

The TCA9554 powers up with pins configured as inputs. Firmware performs these
writes before starting BACnet:

1. output-latch register = `0x00`;
2. polarity register = `0x00`;
3. configuration register = `0x00` to make all pins outputs.

This ordering prevents a stale/high output latch from being applied when the
pins change direction. All relays remain off unless persistent relay restore
was explicitly enabled. With restore enabled, the saved mask is applied only
after the off sequence.

## Input behavior

Inputs are sampled every 20 ms and accepted after three matching samples.
`input_invert_mask` defaults to `255`, making all channels active-low. A set bit
inverts that channel; clear the bit only after verifying the electrical signal
on that channel.

## Electrical safety

- Have qualified personnel handle mains and other hazardous relay circuits.
- De-energize and verify circuits before wiring or first firmware testing.
- Keep controlled loads disconnected for the first flash and point checkout.
- Respect the manufacturer's contact ratings, load type, isolation,
  overcurrent protection, enclosure, conductor, and environmental guidance.
- A BACnet Present_Value indicates a command, not mechanically verified
  contact position.
- Do not use this firmware as a life-safety, emergency-stop, or sole protective
  control.

Pin mapping is compile-time. Do not flash a different Waveshare 8DI/8RO/8DO
variant merely because its enclosure looks similar.
