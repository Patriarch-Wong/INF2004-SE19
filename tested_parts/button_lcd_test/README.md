# SD-card video player for the ST7789 LCD

For H.264/AAC playback while receiving into RAM, with no receiver SD card,
use the separate [wireless_stream project](wireless_stream/README.md).

For the two-board ESP-NOW transfer with receiver video and GPIO8 audio,
use the separate [wireless_av project](wireless_av/README.md).

Plays a converted video clip from the
ESP32-S3 N16R8 board's built-in SD slot. The converted file is
`media/video.lcdv`: 240 x 136 pixels, 12 fps, 911 frames (75.92 seconds),
3,400,387 bytes (3.24 MiB). Playback is silent and centered on the
240 x 240 screen with black bars above and below.

The `media/` folder is not committed to Git. Generate `media/video.lcdv`
first using [Generate the video](#generate-the-video).

The video loops automatically at boot once the file is on the card.
Press the GPIO4 button to pause/resume. The external LED lights while
pressed. The firmware streams one compressed JPEG frame at a time;
the full video is not stored in firmware or RAM.

## Wiring

| Component pin | ESP32-S3 connection |
| --- | --- |
| Button, first contact | GPIO4 |
| Button, other contact | GND |
| LED anode (+) | GPIO5 through a 330 ohm resistor |
| LED cathode (-) | GND |
| LCD GND | GND |
| LCD VCC | 3.3 V, for a 3.3 V compatible module |
| LCD SCL / SCK | GPIO10 |
| LCD SDA / MOSI | GPIO11 |
| LCD RES / RST | GPIO12 |
| LCD DC | GPIO13 |
| LCD BLK | Module-specified backlight power/enable |
| Built-in SD CMD | GPIO38 |
| Built-in SD CLK | GPIO39 |
| Built-in SD D0 | GPIO40 |

The SD mapping comes from the verified `../codec_sdtest` and
`../speaker_test` projects. SD uses 1-bit SDMMC at 20 MHz, independent of
LCD SPI. The seven-pin LCD has no CS: `lcd_cs_pin = -1`, SPI mode 3,
10 MHz. All settings are in `include/app_config.h`.

Disconnect power before changing wiring. For BLK, use direct 3.3 V only
if the module supports it with onboard current limiting or a logic enable.
A raw LED backlight needs its specified current limiting/driver.
The SD card must already have a supported FAT filesystem. Firmware does
not format the card.

## Build, upload, copy, play

From this folder, using the board's UART/COM connector:

```sh
pio run
pio run -t upload --upload-port PORT
```

If download mode is required, hold BOOT, tap RESET, release BOOT, then
upload. Press RESET after upload. Close any serial monitor before copying:

```sh
python3 tools/send_video.py --port PORT
```

The sender needs `pyserial` (`pip install pyserial`). PlatformIO's own
Python environment already includes it.

It transfers acknowledged 4096-byte chunks to `/video.lcdv.part`, closes
and reopens the file, verifies SHA-256 from SD, then renames it to
`/video.lcdv`. A matching existing video is verified and reused. A different
existing video is preserved and reported as an error. A failed transfer
may leave the staging file; the next transfer retries that staging file.
The sender starts playback, waits for one complete loop, and exits while
playback continues.

Alternatively, copy `media/video.lcdv` to the root of the SD card with a
card reader, then insert the card and reset the board.

Without an SD card, stream the clip frame by frame instead:

```sh
python3 tools/stream_video.py --port PORT --loops 1
```

The sender paces frames at the clip's frame rate and keeps at most two
in flight: one decoding, one queued in the board's serial buffer. Each
`frame <bytes>` command is followed by one JPEG; the board decodes it
straight to the LCD and replies `SHOWN <count> <decode_us>`. Overdue
frames are skipped on the Mac. Nothing is stored on the board.

For the native USB connector, upload the `esp32-s3-n16r8-usb` environment
and pass its port to the sender. Both environments use 921600 baud.
`pio device list` shows available ports.

## Generate the video

Download a source video you have the rights to use with `yt-dlp`
(replace `<VIDEO_URL>`), or place your own file at `media/source.mp4`:

```sh
yt-dlp --no-playlist --no-cache-dir \
    -f 'bv*[ext=mp4]+ba[ext=m4a]/b[ext=mp4]' --merge-output-format mp4 \
    -o 'media/source.%(ext)s' '<VIDEO_URL>'
python3 tools/convert_video.py media/source.mp4
```

If the default format returns HTTP 403, list formats with
`yt-dlp -F '<VIDEO_URL>'` and pass HLS video and audio IDs instead,
as `-f VIDEO_ID+AUDIO_ID`. Keep an audio track in the download: the
wireless variants reuse the same `media/source.mp4` for audio.

The converter needs `ffmpeg` and Python's standard library. It preserves
aspect ratio inside 240 x 136, removes audio, and encodes baseline JPEG
frames. `--fps` controls frame rate and `--quality` controls JPEG quality
(2 is highest quality, 31 lowest; default 6). Source downloads and
converted clips in `media/` are ignored by Git. The original still-image files remain available, but the player
no longer displays them.

The `.lcdv` format is little-endian: 8-byte magic `LCDV0001`, 16-bit width,
height, fps, reserved zero, then 32-bit frame count and maximum JPEG size.
The 24-byte header is followed by repeated 32-bit JPEG lengths and JPEG
payloads. The player checks bounds and dimensions and stops on SD or JPEG
errors. It skips overdue frames to keep the intended playback duration.

## Diagnostics

Use `pio device monitor` at 921600 baud after the sender has exited.
Commands: `status` reports SD readiness and playback counters; `play`
restarts the clip; `pause` toggles pause/resume. `LOOP` reports completed
loops with decoded/skipped frame totals. These serial messages are for
file transfer and diagnostics; video rendering goes to the LCD.

If the LCD reports `Upload video over USB`, transfer the converted file.
`SD mount failed` means the card, its filesystem, or the SD connection needs
checking. A completely dark screen calls for checking LCD/backlight power.
Initialization and successful decoding cannot confirm physical LCD output.

Both PlatformIO environments build successfully. All 911 converted JPEG
frames passed a host decode check; malformed/truncated containers were
rejected by the sender.

Hardware check on 2026-09-25: copied 3,400,387 bytes to the onboard SD card
and verified SHA-256
`670c4e3193cb285b91d73c0f51be35525363b1db9693b7e97ba702b2b499b151`.
The board completed one loop: 902 decoded frames and 9 skipped to maintain
timing, with no SD or JPEG errors. The longest read/decode/draw took
89,340 microseconds. Pause/resume events were received during the check.
Playback was left running. Physical image quality still needs visual
confirmation on the LCD.
