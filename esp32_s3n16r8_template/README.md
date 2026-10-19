# ESP32-S3 N16R8 PlatformIO template

Arduino starter for an ESP32-S3 with **16 MB Quad-SPI flash and 8 MB Octal-SPI PSRAM**.
Uses PlatformIO's `esp32-s3-devkitc-1` board with explicit N16R8 overrides.
The Espressif32 platform is pinned to `7.1.3` (Arduino-ESP32 2.0.17 family).

## Start a new project

Copy this folder to a new project directory, excluding generated files. From this
template's directory, replace `../my-new-project/` with your destination:

```sh
rsync -av --exclude='.pio/' --exclude='.git/' --exclude='.DS_Store' \
  --exclude='sdkconfig.*' --exclude='.vscode/c_cpp_properties.json' \
  --exclude='.vscode/launch.json' --exclude='.vscode/ipch/' \
  --exclude='.vscode/.browse.c_cpp.db*' --exclude='include/secrets.h' \
  ./ ../my-new-project/
```

Open the new directory in VS Code with the PlatformIO extension. Change
`projectName` in `include/app_config.h`, then add your application to `src/main.cpp`.
Put shared headers in `include/`, project libraries in `lib/` if needed, and
third-party dependencies in `lib_deps` in `platformio.ini`.

## Build, upload, and monitor

The default environment uses `Serial` through a USB-to-UART bridge. On boards
with two connectors, this is usually the one labeled **UART** or **COM**.

```sh
pio run
pio run -t upload
pio device monitor
```

For the ESP32-S3's **native USB** connector, select the USB environment for all
three commands:

```sh
pio run -e esp32-s3-n16r8-usb
pio run -e esp32-s3-n16r8-usb -t upload
pio device monitor -e esp32-s3-n16r8-usb
```

You can make native USB the default by changing `default_envs` in
`platformio.ini` to `esp32-s3-n16r8-usb`. Native USB uses hardware USB Serial/JTAG
(`ARDUINO_USB_MODE=1`) with CDC on boot enabled.

Use `pio device list` to find your port. If automatic detection chooses the wrong
device, pass `--upload-port PORT` to the upload command and `--port PORT` to the
monitor command. Monitor baud is 115200. If a board will not enter download mode,
hold **BOOT**, press and release **RESET**, then release **BOOT** and retry upload.
After the initial native USB upload, a reset or port reselection may be necessary.

The program reports detected flash and PSRAM sizes at startup and prints a
heartbeat every five seconds. Expected sizes are `16777216` and `8388608` bytes.
Reset with the monitor open to see the startup report. Startup waits at most two
seconds for serial, so the application can run without a connected computer.

## Memory and storage

`qio_opi` selects Quad-SPI flash and Octal-SPI PSRAM; `BOARD_HAS_PSRAM` enables
PSRAM initialization. The custom partition table covers the full 16 MB:

| Partition | Size | Purpose |
| --- | --- | --- |
| nvs | 20 KiB | Preferences / nonvolatile settings |
| otadata | 8 KiB | OTA boot selection |
| app0, app1 | 6.25 MiB each | Two firmware slots |
| spiffs | 3.375 MiB | LittleFS storage, retaining the conventional partition label |
| coredump | 64 KiB | Reserved core dump partition |

Two firmware slots allow adding OTA later; this starter does not implement an OTA
service. LittleFS is selected for optional filesystem images, but is not mounted
or formatted automatically. To use it, create `data/`, add files, run
`pio run -t uploadfs`, and mount with `LittleFS.begin(false)` in your application.
Use `-e esp32-s3-n16r8-usb` with `uploadfs` when using that environment.
Uploading a filesystem image replaces existing files in that partition.

The existing `sdkconfig.esp32-s3-n16r8` file is left untouched and ignored: this
Arduino-only build uses the framework's prebuilt configuration. Edit
`platformio.ini`, not `sdkconfig.*`, for this template's board settings.

No LED or peripheral pins are assumed: N16R8 specifies memory capacity, not a
particular development board's pinout. Check your board schematic before adding
peripherals. Keep credentials in the ignored `include/secrets.h` if needed.

## References

- [PlatformIO ESP32-S3-DevKitC-1 board](https://docs.platformio.org/en/latest/boards/espressif32/esp32-s3-devkitc-1.html)
- [Espressif USB CDC and DFU guide](https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/cdc_dfu_flash.html)
- [Arduino-ESP32 2.0.17 partition layout](https://github.com/espressif/arduino-esp32/blob/2.0.17/tools/partitions/default_16MB.csv)
