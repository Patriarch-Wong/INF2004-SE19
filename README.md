# INF2004 — Team SE19

ESP32-S3 embedded systems project and component test bench, submitted in
partial fulfilment of the requirements for **INF2004 Embedded Systems
Programming**.

All firmware targets the **ESP32-S3 N16R8** (16 MB Quad-SPI flash, 8 MB
Octal-SPI PSRAM) using PlatformIO with the Arduino framework, pinned to
`espressif32@7.1.3` (Arduino-ESP32 2.0.17).

## Team

| Name | Student ID | Role / contribution |
| --- | --- | --- |
| | | |
| | | |
| | | |
| | | |

## Repository structure

```text
INF2004-SE19/
├── esp32_s3n16r8_template/   Reusable PlatformIO starter for the N16R8 board
└── tested_parts/             One standalone PlatformIO project per component test
    ├── button_test/          Debounced tactile button + LED
    ├── button_lcd_test/      ST7789 LCD video player from SD
    │   ├── wireless_av/      ESP-NOW file transfer to SD, then A/V playback
    │   └── wireless_stream/  H.264/AAC playback while receiving into RAM
    ├── codec_test/           Opus encode/store/decode round trip
    ├── codec_sdtest/         Opus + onboard SDMMC with reusable C/C++ APIs
    ├── espnow_test/          Two-board ESP-NOW exchange with MAC whitelist
    ├── espnow_sd_test/       One-way ESP-NOW transfer saved and verified on SD
    ├── lora_test/            Core1121 868 MHz LoRa PING/PONG
    └── mictospeaker_test/    I2S mic → ESP-NOW ADPCM → PDM speaker
```

Every directory containing a `platformio.ini` is an independent project with
its own README covering wiring, configuration, build/upload steps, and
measured results.

## Hardware

| Component | Used in |
| --- | --- |
| ESP32-S3 N16R8 development board (x2 for radio tests) | All |
| Onboard SD slot (1-bit SDMMC: CMD 38, CLK 39, D0 40) | `codec_sdtest`, `espnow_sd_test`, `button_lcd_test` |
| 4-pin tactile pushbutton + LED with 330 Ω resistor | `button_test`, `button_lcd_test` |
| ST7789 240 × 240 SPI LCD | `button_lcd_test` |
| Waveshare Core1121 LoRa module + 868 MHz antenna (x2) | `lora_test` |
| ICS43434 I2S MEMS microphone | `mictospeaker_test` |
| YwRobot powered speaker module | `mictospeaker_test` |

Pin assignments differ between tests; check the project README and its
`include/app_config.h` before wiring.

## Getting started

1. Install [VS Code](https://code.visualstudio.com/) with the
   [PlatformIO extension](https://platformio.org/install/ide?install=vscode),
   or the [PlatformIO Core CLI](https://docs.platformio.org/en/latest/core/installation/index.html).
2. Open a single project directory (for example `tested_parts/codec_test`),
   not the repository root.
3. Build, upload, and monitor:

   ```sh
   cd tested_parts/codec_test
   pio run
   pio run -t upload
   pio device monitor
   ```

Each project provides two environments:

| Environment | Serial connection |
| --- | --- |
| `esp32-s3-n16r8` (default) | USB-to-UART bridge (connector labelled **UART** / **COM**) |
| `esp32-s3-n16r8-usb` | Native USB Serial/JTAG |

Select one with `-e <environment>`. If upload cannot enter download mode,
hold **BOOT**, tap **RESET**, release **BOOT**, then retry. Several radio
projects set `before_reset = no_reset` and always need this manual step.

To start a new component, copy `esp32_s3n16r8_template/` as described in
its [README](esp32_s3n16r8_template/README.md).

## Test summary

| Project | Result |
| --- | --- |
| [button_test](tested_parts/button_test/) | UART and native USB builds pass; hardware validation pending |
| [button_lcd_test](tested_parts/button_lcd_test/) | SD video playback with pause button; wireless A/V and streaming variants transfer with CRC32 verification |
| [codec_test](tested_parts/codec_test/) | Three Opus hardware round trips passed over UART |
| [codec_sdtest](tested_parts/codec_sdtest/) | Three Opus + SD hardware round trips passed |
| [espnow_test](tested_parts/espnow_test/) | Two-way reception confirmed; unknown-device rejection not tested |
| [espnow_sd_test](tested_parts/espnow_sd_test/) | 4096-byte transfer passed with SD read-back, CRC32, zero queue drops |
| [lora_test](tested_parts/lora_test/) | Hardware PING/PONG confirmed; latest fix awaits hardware retest |
| [mictospeaker_test](tested_parts/mictospeaker_test/) | Live audio streams between boards; packet gaps and mic noise remain |

Detailed logs and measurements are in each project's README and `results/`
directory.

## Coding standard

Firmware follows an embedded subset of the C++ Core Guidelines: RAII and
checked status returns, no exceptions or RTTI. Formatting is BARR-style:
`snake_case`, four-space indentation, Allman braces, 80-column limit.
BARR-C applies where relevant to C code.

## Third-party components

- [Arduino-ESP32](https://github.com/espressif/arduino-esp32) and
  [ESP-IDF](https://github.com/espressif/esp-idf), via PlatformIO.
- Espressif [`esp_h264`](https://github.com/espressif/esp-h264-component) and
  [`esp_audio_codec`](https://github.com/espressif/esp-adf-libs), vendored
  unmodified under `tested_parts/button_lcd_test/wireless_stream/lib/` and
  `tested_parts/mictospeaker_test/sd_file_player/lib/`. See the bundled
  licence files; the audio codec is licensed for use with Espressif
  products only.
- Libraries pinned in `lib_deps`: Adafruit ST7735 and ST7789 Library,
  TJpg_Decoder, RadioLib, and
  [arduino-libopus](https://github.com/pschatzmann/arduino-libopus).
