# ESP-NOW whitelist test

Two ESP32-S3 N16R8 boards exchange numbered ESP-NOW packets once per second
on Wi-Fi channel 1. Both run the same firmware and accept packets only from
the other configured station MAC address. No router is needed.

Two-way reception was verified on 2026-09-22: both boards received sequences
1–7. Rejection of an unknown device has not been tested with this firmware;
`e2e_security_test` stage 2 tested the same kind of MAC allowlist with an
unlisted sender (`board3-unknown`), and its frames were dropped. Packets are
unencrypted; the MAC whitelist is a filter, not authentication.

## Hardware and configuration

- Two ESP32-S3 boards with 16 MB flash and 8 MB PSRAM (N16R8).
- PlatformIO with Arduino, pinned to `espressif32@7.1.3`.
- USB-to-UART connections for the tested setup, using `esp32-s3-n16r8`.
- Serial baud: 115200. Monitor DTR and RTS: both 0.

| Board | Wi-Fi station MAC |
| --- | --- |
| 1 | `E0:72:A1:D9:A4:94` |
| 2 | `E0:72:A1:D8:E2:10` |

Commands below use `BOARD1_PORT` and `BOARD2_PORT`. Run `pio device list`
to find them. To identify a physical board, unplug the other one and list
the ports again.

`include/espnow_config.h` contains both allowed MAC addresses, the channel, and
the send interval. When replacing boards, update both station MACs and flash
both boards again. The firmware prints its own station MAC at startup. If its
MAC is not listed or no valid peer is configured, it leaves ESP-NOW disabled.

## Build and upload

Run commands from this project directory:

```sh
pio run -e esp32-s3-n16r8
```

Close the target board's serial monitor before uploading. The project sets
`board_upload.before_reset = no_reset`, so enter download mode manually before
**each** upload: hold **BOOT**, press and release **RESET**, then release **BOOT**.

Upload to board 1:

```sh
pio run -e esp32-s3-n16r8 -t upload --upload-port BOARD1_PORT
```

Repeat the button sequence on board 2, then upload:

```sh
pio run -e esp32-s3-n16r8 -t upload --upload-port BOARD2_PORT
```

Press **RESET** without holding BOOT after uploading to start the application
if it does not start automatically.

## Monitor and confirm reception

Open one terminal per board:

```sh
pio device monitor -e esp32-s3-n16r8 --port BOARD1_PORT --baud 115200 --dtr 0 --rts 0
```

```sh
pio device monitor -e esp32-s3-n16r8 --port BOARD2_PORT --baud 115200 --dtr 0 --rts 0
```

DTR and RTS are also set to 0 in `platformio.ini`. Output appeared on both
boards when these lines were explicitly disabled during troubleshooting.
Press **Ctrl+C** to close a monitor. Press RESET with the monitor open to see
the startup messages.

Example output on board 1:

```text
ESP-NOW whitelist test
Wi-Fi station MAC: E0:72:A1:D9:A4:94
Ready on channel 1; sending once per second.
TX 1 | queued
RX E0:72:A1:D8:E2:10 | sequence 1
```

`TX ... queued` means the local send request was accepted. `RX ... sequence ...`
on the other board confirms reception. Look for RX on **both** boards to verify
two-way communication.

## Troubleshooting

| Symptom | Action |
| --- | --- |
| Blank monitor | Restart it with `--dtr 0 --rts 0`, then press RESET without BOOT. Confirm the port and baud. |
| `Wrong boot mode` or `No serial data received` during upload | Close the monitor, verify the port, and repeat BOOT + RESET on the board attached to that port. |
| `Error 2` | Read the preceding `A fatal error occurred` line; Error 2 alone does not identify the cause. |
| `ESP-NOW stopped` | The current source repeats the failed setup step every two seconds. Check its message and printed station MAC. |
| TX but no RX | Confirm both boards are running, both have the same whitelist and channel, and the whitelist uses station MACs. |
| Port busy | Close other monitors or upload processes using that port. |

## Project files

- `src/main.cpp`: station setup, receive filtering, packet queue, periodic sending,
  and serial diagnostics. Receive callbacks queue valid packets for the main loop.
- `include/espnow_config.h`: shared whitelist and radio/test settings.
- `include/app_config.h`: serial baud and startup wait settings.
- `platformio.ini`: board, memory, upload, and monitor configuration.
- `partitions.csv`: custom 16 MB flash partition layout.

The board configuration uses `qio_opi` for Quad-SPI flash and Octal-SPI PSRAM.
The partition layout reserves two 6.25 MiB application slots, 3.375 MiB for
LittleFS, and NVS, OTA metadata, and coredump regions. This test does not use
OTA or mount LittleFS.

The alternative `esp32-s3-n16r8-usb` environment routes `Serial` to native USB
Serial/JTAG with CDC on boot enabled. Use it only with the native USB connector
and its detected port; the tested bridge ports above use the default environment.

## Git hygiene

Keep source, configuration, the whitelist, partition table, and shared VS Code
extension recommendations in version control. `.gitignore` excludes build
output, generated editor files, logs, local caches, and secrets. MAC addresses
are device identifiers, not secret keys; the whitelist stays tracked so the
same firmware can be reproduced for both boards.
