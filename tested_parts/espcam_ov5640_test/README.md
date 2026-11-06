# OV5640 camera test

Standalone PlatformIO / Arduino project that checks an OV5640 camera on an
ESP32 camera board:

1. **Sensor check:** the camera starts and reports PID `0x5640` over SCCB.
2. **Self-test (serial):** for each of ten frame sizes from QVGA (320×240)
   to QSXGA (2560×1920, 5 MP), throw away frames until the new size arrives,
   then capture 8 frames. Each frame must be a complete JPEG (SOI and EOI
   markers) whose SOF header gives the requested width and height; the
   driver's own width/height fields are not trusted. Prints fps and JPEG
   sizes per resolution.
3. **Live view over the USB cable:** the board sends JPEG frames on its
   serial port and `tools/serial_view.py` shows them in an `ffplay` window
   on the computer. No Wi-Fi and no browser. Each frame is checked with
   CRC-32 before it is shown.

## Boards

| Environment | Board | Notes |
| --- | --- | --- |
| `esp32s3-cam` (default) | ESP32-S3 N16R8 CAM board, Freenove ESP32-S3-WROOM CAM pinout | Serial on the UART USB-C connector at 2,000,000 baud |
| `esp32s3-cam-usb` | Same | Serial on the native USB connector (faster live view) |
| `ai-thinker-esp32cam` | AI-Thinker ESP32-CAM (ESP32, 4 MB PSRAM) | Flash through an ESP32-CAM-MB or USB-UART adapter |

Pin maps are in `include/app_config.h`. If the board is a different S3 CAM
clone and camera init fails with `0x105` or `0x20004`, compare its schematic
against the `CAM_BOARD_S3_WROOM` map:

| Signal | ESP32-S3 WROOM CAM | AI-Thinker |
| --- | --- | --- |
| PWDN / RESET | – / – | 32 / – |
| XCLK | 15 | 0 |
| SIOD / SIOC | 4 / 5 | 26 / 27 |
| D7..D0 (Y9..Y2) | 16 17 18 12 10 8 9 11 | 35 34 39 36 21 19 18 5 |
| VSYNC / HREF / PCLK | 6 / 7 / 13 | 25 / 23 / 22 |

PSRAM is required: frame buffers are sized for 5 MP JPEG (about 960 KB
each, two buffers).

## Build and run

```sh
cd tested_parts/espcam_ov5640_test
pio run                       # or: pio run -e esp32s3-cam-usb
pio run -t upload
pio device monitor            # 2,000,000 baud; close it before the viewer
```

The UART build runs Serial at 2,000,000 baud so frames fit through the
CH343 USB-to-UART bridge; the boot ROM messages at 115200 show as garbage,
which is expected. If the bridge cannot do 2 Mbaud, change `SERIAL_BAUD` in
`platformio.ini` and pass the same `--baud` to the viewer.

For manual download mode on the S3 board, hold BOOT, press and release RESET,
then release BOOT before upload. Press RESET after upload to start.

## Expected serial output

```text
OV5640 camera test | ESP32-S3 WROOM CAM
PSRAM 8192 KB (free ...) | flash 16 MB
Camera init ... ms | sensor PID 0x5640 (OV5640) SCCB 0x3c
PASS sensor is OV5640

Self-test: 8 frames per size, JPEG quality 12
  size  WxH        settle ok   fps    avg KB  min-max KB  result
  QVGA   320x240    ...
  ...
  QSXGA 2560x1920   ...
Self-test PASS: 10/10 sizes passed | PSRAM free ... KB
Commands: l live frames on | x live off | c one frame | t self-test
```

Serial commands: `l` start sending frames, `x` stop, `c` send one frame,
`t` rerun the self-test, `0`–`9` set the frame size (0 QVGA … 9 QSXGA),
`v` vertical flip, `m` mirror, `s` status, `h` help.

## Live view over USB

Close `pio device monitor` first (only one program can hold the port), then
use PlatformIO's Python, which already has pyserial. `ffplay` comes with
`brew install ffmpeg`.

```sh
PY=~/.platformio/penv/bin/python
$PY tools/serial_view.py                   # live window at VGA
$PY tools/serial_view.py --size 6          # live window at UXGA
$PY tools/serial_view.py --snap --size 9   # save one 5 MP photo to captures/
$PY tools/serial_view.py --save captures   # live, and save every frame
$PY tools/serial_view.py --no-window       # just fps / KB/s / bad counts
```

The port is found automatically when only one board is plugged in;
otherwise pass `--port /dev/cu.…`. Opening the port does not reset the board.
Board text (self-test, status) is printed in the terminal, along with fps,
KB/s and a count of frames that failed the CRC or size check and were
dropped. Press Ctrl-C to stop; live mode is switched off on exit.

The cable limits the frame rate: about 195 KB/s at 2 Mbaud on the UART
connector, so roughly 5–10 fps at VGA. The native USB build (`esp32s3-cam-usb`
on the other connector) is several times faster.

Wire format, little-endian: `"OVJ1"`, JPEG length (u32), width (u16),
height (u16), zlib CRC-32 of the JPEG (u32), then the JPEG bytes. Text lines
can appear between frames. Driver warnings are muted while live mode is on
so they cannot land inside a frame.

Host tests for the frame parser:

```sh
python3 -m unittest discover -s tests
```

## Optional Wi-Fi viewer

Off by default. Set `wifi_enabled = true` in `include/app_config.h` and the
board also creates the access point `ov5640-test` (password `camtest123`)
with a browser page at <http://192.168.4.1/> (stream, size, flip, snapshot).
`tools/grab.py` then saves and checks one snapshot per size over Wi-Fi.

## Pass criteria

- Sensor PID is `0x5640`.
- Self-test: 8/8 valid frames at every size, 10/10 sizes.
- `tools/serial_view.py` shows a correct, non-garbled image with 0 bad
  frames, and `--snap --size 9` saves a 2560×1920 photo.

## Notes

- Autofocus OV5640 modules need an AF firmware blob loaded over SCCB; the
  `esp32-camera` driver does not do this, so AF modules stay at their fixed
  focus here.
- The image may be upside down depending on how the module is mounted; use
  `v` / `m` or the page buttons.
- High resolutions are slow (a few fps at 5 MP) and are the most likely to
  show `cam_hal: FB-OVF` or `NO-EOI` warnings if PCLK is too fast for the bus;
  lowering `xclk_hz` in `app_config.h` to 10 MHz is the first thing to try.

## Results

Builds for all three environments; frame parser host tests pass; the
viewer was checked end to end against a simulated board on a virtual serial
port (78 real JPEG frames saved and decoded, a corrupted frame dropped,
board text passed through).

Hardware, 2026-11-06, ESP32-S3 N16R8 CAM (`esp32s3-cam`): sensor detected as
OV5640, self-test PASS 10/10 sizes, 8/8 valid frames each; QXGA 4.5 fps
(~102 KB/frame), QSXGA 4.5 fps (~158 KB/frame); 6,253 KB PSRAM free after.
First serial live-view run got 0 frames: the start command was sent while the
board was booting and was lost. The viewer now resends it until frames
arrive; live view not yet confirmed on hardware.
