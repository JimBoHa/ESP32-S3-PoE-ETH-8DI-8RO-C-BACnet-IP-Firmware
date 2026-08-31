# Third-party notices

This project links third-party components into the firmware image.

## bacnet-stack 1.6.0

Source: <https://github.com/bacnet-stack/bacnet-stack>

Pinned commit: `9bc3cfa07aab98852de432fa24079f4b4b6b7eed`

The upstream project applies licenses per source file. Most BACnet library
files used here are `GPL-2.0-or-later WITH GCC-exception-2.0`; some template,
service, or utility files are MIT or Apache-2.0. Exact SPDX identifiers appear
in each source file. Complete license texts are in
`third_party/bacnet-stack/license/` and are copied into packaged releases.

No local modifications are made inside the pinned submodule. The ESP32 port
adapter and firmware application outside the submodule are Apache-2.0.

## Espressif ESP-IDF 5.5.4

Source: <https://github.com/espressif/esp-idf/tree/v5.5.4>

ESP-IDF contains components under several permissive licenses, identified by
SPDX headers and component metadata. Principal framework code is Apache-2.0.
The linked dependency set includes FreeRTOS, lwIP, mbedTLS, and cJSON; retain
the applicable notices when redistributing firmware. The release packager
copies the ESP-IDF framework license and the relevant FreeRTOS, lwIP, mbedTLS,
cJSON, http-parser, and C-library license texts into `licenses/`. See the
ESP-IDF source tree and per-component SPDX metadata for exact terms.

## Standards and marks

BACnet is a registered trademark of ASHRAE. Use of the protocol name does not
imply BTL certification or listing. Waveshare and Espressif names identify the
target hardware and SDK; neither organization endorses this firmware.
