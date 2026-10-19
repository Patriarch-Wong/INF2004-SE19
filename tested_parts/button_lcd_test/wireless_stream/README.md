# H.264/AAC playback while receiving into RAM

## Upload the optimized firmware manually

**Flash both boards.** The optimized packet protocol is incompatible with
the earlier streaming firmware. The existing `/clip.lcdstream` on sender
SD can be reused; the media format has not changed.

Run these commands from `button_lcd_test`. Close serial monitors and any
benchmark script first. This project uses `before_reset = no_reset`, so
put each board into download mode yourself: **hold BOOT, tap RESET, then
release BOOT** before running its upload command.

Sender, port ending in **2791**:

```sh
pio run -d wireless_stream -e sender -t upload \
    --upload-port SENDER_PORT
```

Press the sender's **RESET** button after a successful upload. Then put
the receiver into download mode and upload it, port ending in **5891**:

```sh
pio run -d wireless_stream -e receiver -t upload \
    --upload-port RECEIVER_PORT
```

Press the receiver's **RESET** button after uploading. Both builds default
to a **6 Mbps PHY rate**, with a **32-packet sender window** and receiver
autoplay enabled. PHY rate is not the useful file-transfer throughput.
Keep the boards near each other for the initial speed tests.

Press the sender's GPIO4 button for normal streaming, or follow
[Benchmark the transfer](#benchmark-the-transfer) below to measure speed.
Commands use `SENDER_PORT` and `RECEIVER_PORT`; find them with
`pio device list` and match each board's startup MAC against
[Wiring and board identification](#wiring-and-board-identification).

## Overview

This separate PlatformIO project streams a prerecorded clip over ESP-NOW.
The sender reads from its SD card; the receiver saves incoming compressed
records in PSRAM and starts playback after buffering three seconds of
both audio and video. The receiver does **not** need an SD card.

```text
Mac -> sender SD -> ESP-NOW -> receiver PSRAM
                                  |
                       three seconds buffered
                                  |
                        H.264/AAC decoders
                                  |
                         ST7789 + GPIO8 audio
```

If reception falls behind, audio and video pause until the buffer refills.
There is no guaranteed live latency or uninterrupted frame rate. A complete,
verified clip loops from RAM without another send. Resetting or removing
receiver power loses the clip. Press the sender button to transmit again.

The earlier [wireless_av project](../wireless_av/README.md) remains available
for receiving a complete JPEG/mu-law file onto SD before playing it.
The two protocols and file formats are different; flash **both boards**
with this project's firmware to use RAM streaming.

## Prepared clip

| Property | Value |
| --- | --- |
| File | `../media/clip.lcdstream` |
| Size | 1,046,349 bytes, about 77.3% smaller than `clip.lcdav` |
| Video | H.264 constrained baseline, 240 x 136, 12 fps, 911 frames |
| Audio | AAC-LC, 16 kHz mono, target 32 kbit/s |
| Duration | 75.917 seconds |
| Display | Centered on a 240 x 240 LCD with black bars |
| Initial buffer | Three seconds, 39,872 bytes at 224-byte packet boundaries |

The clip is not committed to Git; build it with
[Regenerate the compressed clip](#regenerate-the-compressed-clip).
Resolution and frame rate are preserved; compression is lossy. The startup
buffer duration describes media time, not guaranteed wall-clock wait time.
Conversion and host decoding passed; physical picture/sound quality and
actual ESP32-S3 playback performance still need checking.

The custom `.lcdstream` container holds timestamped H.264 access units and
AAC ADTS frames in playback order. It avoids an MP4 demuxer on the board.
An ordinary `.mp4` cannot be renamed and used with this firmware.

## Wiring and board identification

| Role | Station MAC |
| --- | --- |
| Sender | `E0:72:A1:D7:F6:50` |
| Receiver | `E0:72:A1:D9:A4:94` |

The receiver port was identified by elimination, not by reading its MAC.
Each firmware checks its board's MAC at startup. Confirm ports using
`pio device list`. Pins and MACs are in [app_config.h](include/app_config.h).

| Receiver signal | GPIO |
| --- | --- |
| LCD RES | 10 |
| LCD SDA / MOSI | 11 |
| LCD SCL / SCK | 12 |
| LCD DC | 9 |
| LCD BLK | 14 |
| Amplified speaker signal | 8 |
| Pause/resume button | 4 to GND |
| Optional LED | 5 through a series resistor |

LCD: ST7789, 240 x 240, no CS, SPI mode 3, 10 MHz. GPIO14 is a
backlight-enable signal for a compatible module. Audio uses I2S PDM on
GPIO8 and the same compatible amplified input as the earlier project;
it does not directly drive a bare speaker or a three-wire I2S DAC.

The sender needs a FAT-formatted SD card in its built-in slot (CMD38,
CLK39, D0=40) and a button between GPIO4 and GND. Only the sender mounts
SD. See the [macOS formatting steps](../wireless_av/README.md#prepare-both-sd-cards-on-macos),
applying them to the sender card for this project.

## Build, flash, copy, and send

Run from the parent `button_lcd_test` folder:

```sh
pio device list
pio run -d wireless_stream -e sender -e receiver
```

Close serial monitors. If required, hold BOOT, tap RESET, then release BOOT
before uploading to each board:

```sh
pio run -d wireless_stream -e sender -t upload \
    --upload-port SENDER_PORT
pio run -d wireless_stream -e receiver -t upload \
    --upload-port RECEIVER_PORT
```

Press RESET after flashing. The receiver should display `Waiting for stream`.
For native USB connectors use environments `sender-usb` / `receiver-usb`
and their corresponding ports.

Copy the prepared clip to the sender using a Python environment containing
`pyserial`:

```sh
python3 \
    wireless_stream/tools/send_clip.py \
    --port SENDER_PORT media/clip.lcdstream
```

The script validates the stream, copies it to `/clip.lcdstream` on sender
SD, and verifies the saved bytes with SHA-256. It does not start a radio
transfer. Before uploading it checks `identity` and requires
`FIRMWARE wireless_stream sender`. Older builds without this command must
be updated; the script stops before sending file data to them. It prints
the selected filename and byte count before opening the serial port.
A matching existing file is reused; a different existing file
is preserved and reported as an error. Rename/remove that specific sender
file before copying a replacement. Older `.lcdav` files are untouched.

With both boards powered, press the **sender GPIO4 button**. The receiver
shows `Buffering stream...`, then begins playback while reception continues.
The **receiver GPIO4 button** pauses/resumes playback, including during
reception. Pausing playback does not pause the download.

## Next steps after uploading the receiver

If the receiver firmware has already been uploaded successfully, continue
with these steps from `button_lcd_test`.

1. Connect the sender with its FAT32 SD card inserted. Close its serial
   monitor and upload the matching streaming firmware:

   ```sh
   pio run -d wireless_stream -e sender -t upload \
       --upload-port SENDER_PORT
   ```

2. Press RESET on the sender, then copy the compressed clip:

   ```sh
   python3 \
       wireless_stream/tools/send_clip.py \
       --port SENDER_PORT media/clip.lcdstream
   ```

   Wait for `H264/AAC stream verified on sender SD.`

3. Keep both boards powered. Open the receiver monitor:

   ```sh
   pio device monitor --port RECEIVER_PORT \
       --baud 921600 --dtr 0 --rts 0
   ```

4. Press the sender's GPIO4 button. The receiver should buffer, then play
   while reception continues. Look for `PLAYING RAM H264/AAC` in the
   monitor. The receiver does not need an SD card. If an `ERROR` appears,
   capture the full message for troubleshooting.

## Serial monitors

Receiver:

```sh
pio device monitor --port RECEIVER_PORT \
    --baud 921600 --dtr 0 --rts 0
```

Sender, in another terminal:

```sh
pio device monitor --port SENDER_PORT \
    --baud 921600 --dtr 0 --rts 0
```

Type a command and press Enter, even if typing is not echoed:

| Board | Command | Action |
| --- | --- |
| Either | `status` | Report transfer state and playback/buffer counters |
| Either | `identity` | Identify firmware project and sender/receiver role |
| Sender | `send` |
| Either | `rate 6` | Set TX PHY rate while idle: 1, 6, 12, 24, or 54 Mbps |
| Sender | `window 32` |
| Receiver | `autoplay 0` |
| Receiver | `autoplay 1` |
| Receiver | `play` |
| Receiver | `pause` |

| Message | Meaning |
| --- | --- |
| `RAM RECEIVER ready; no SD card required` | Receiver initialized |
| `PROGRESS ...` | Acknowledged bytes and average useful transfer rate |
| `TRANSFER ...` | Final sender timing, kB/s, retries, MAC failures, queue drops |
| `RECEIVE ...` | Receiver timing and queue drops |
| `PLAYING RAM H264/AAC ... received=X/Y` | Playback started at X received bytes |
| `BUFFERING; holding video and audio` | Waiting for more complete records |
| `STREAM RESUMED` | Playback buffer refilled |
| `RAM RECEIVED VERIFIED ...` | Entire compressed clip passed CRC32 |
| `STREAM SENT VERIFIED ...` | Sender received final verification ACK |
| `RAM LOOP ...` | Completed clip is replaying from RAM |
| `ERROR ...` | Inspect the attached transfer or decoder error |

## Benchmark the transfer

The original firmware was measured on these two boards with playback
enabled: **1,046,349 bytes in 274.722 seconds, or 3.81 kB/s**. Both ends
verified CRC32 `9b399532`. Playback repeatedly paused to refill its buffer.
This is the baseline, not a measurement of the optimized firmware.
Optimized hardware measurements are pending manual upload to both boards.

After flashing both optimized builds, leave both boards connected and
close serial monitors. The sender must have the prepared clip on SD.
Start with one transfer while playing:

```sh
python3 \
    wireless_stream/tools/benchmark_radio.py \
    --sender SENDER_PORT \
    --receiver RECEIVER_PORT \
    --rates 6 --windows 32 --playback \
    --output /tmp/espnow-playback.json
```

Then compare radio rates and window sizes with playback disabled to
measure transfer capacity:

```sh
python3 \
    wireless_stream/tools/benchmark_radio.py \
    --sender SENDER_PORT \
    --receiver RECEIVER_PORT \
    --rates 6 12 24 54 --windows 8 32 64 --repeats 2 \
    --output /tmp/espnow-sweep.json
```

The script configures both boards, sends the existing clip, and checks
that both ends report the same byte count and whole-file CRC. Each result
includes elapsed time, useful decimal kB/s, settings, and serial logs.
Results are saved after each completed transfer. It stops on an error or
timeout. Serial-port opening may reset these USB-UART boards; the script
waits for startup before sending commands.

Compare repeatable `TRANSFER` rates alongside `retries`, `resent`,
`mac_fail`, and `drops`; the highest PHY rate may not win. Run the best
settings again with `--playback --repeats 3` to check performance while
decoding and drawing. The player needs about **13.8 kB/s on average** for
this clip, plus headroom for bitrate peaks. `SENDING` to final verification
is the radio-transfer time; `command_seconds` also includes loading sender
SD into PSRAM. Startup buffering and decoder setup are separate timings.

At exit the script requests `autoplay 1` for subsequent transfers. Rate
and window settings are temporary and return to **6 Mbps / 32 packets**
after reset. To use a measured winner without resetting, issue `rate N`
on both boards and `window N` on the sender through the serial monitors.

During buffering the LCD holds its last frame; progress is visible over
serial. The sample counters use 16,000 samples per second. Transfer success
does not confirm physical display or speaker output. Ctrl+C exits a monitor;
close it before firmware upload or USB copying to that board.

## Troubleshooting: sender still sends the large file

`SENDING 4615086 bytes` identifies the old 4.62 MB `clip.lcdav` transfer.
The streaming sender uses only `/clip.lcdstream`; the prepared new clip is
1,046,349 bytes. It also enforces a 4 MiB maximum, so it cannot send the
4,615,086-byte old file. Updating the receiver alone does not update the
sender firmware.

From `button_lcd_test`, close the sender monitor and flash the new sender:

```sh
pio run -d wireless_stream -e sender -t upload \
    --upload-port SENDER_PORT
```

Press RESET, then explicitly select the smaller clip:

```sh
python3 \
    wireless_stream/tools/send_clip.py \
    --port SENDER_PORT media/clip.lcdstream
```

Wait for `H264/AAC stream verified on sender SD.`, then press sender GPIO4.
The sender monitor should report:

```text
SENDING 1046349 bytes to E0:72:A1:D9:A4:94
```

The old file may remain on SD. `identity` or `status` on the updated sender
prints `FIRMWARE wireless_stream sender` and `SOURCE /clip.lcdstream`.

## Troubleshooting: AAC/PDM playback failed

This message identifies the audio worker, but by itself does not distinguish
an AAC decoder failure from a PDM output failure. The updated firmware
handles the codec's output-buffer resize requests and decode calls that
consume input without producing PCM. Audio scratch buffers use PSRAM to
leave more worker stack space. The serial log now reports the exact failing
operation, decoder return code, or PDM status and byte count.

Reconnect the receiver, close its monitor, and upload the updated build:

```sh
pio run -d wireless_stream -e receiver -t upload \
    --upload-port RECEIVER_PORT
```

Press RESET and open its monitor:

```sh
pio device monitor --port RECEIVER_PORT \
    --baud 921600 --dtr 0 --rts 0
```

Send again with sender GPIO4. A message such as
`AAC output buffer: 2048 -> 4096 bytes` is an automatic resize, not a failure.
If playback still fails, capture the preceding `ERROR AAC ...` or
`ERROR PDM ...` line as well as `AAC/PDM playback failed`. Buffer handling
has been corrected and all builds pass, but the original hardware failure
still needs a receiver retest to confirm its cause and resolution.

## Regenerate the compressed clip

Requires FFmpeg with `libx264` and the AAC encoder. From the parent folder,
using the `media/source.mp4` described in the
[parent README](../README.md#generate-the-video) for both video and audio:

```sh
python3 wireless_stream/tools/encode_stream.py \
    media/source.mp4 media/source.mp4 \
    --output media/clip.lcdstream --crf 28
```

`--crf` accepts 18-40; a larger value reduces video size and picture quality.
Resolution stays 240 x 136 at 12 fps. The converter inserts AUD boundaries,
SPS/PPS at one-second keyframes, disables B-frames, and uses one reference
frame. It accounts for the native FFmpeg AAC encoder's 1024 priming samples
and trims decoded audio to the video duration. Both elementary streams are
host-decoded before the output is installed.

## Implementation and limits

The receiver reserves the declared compressed file size in PSRAM, capped
at 4 MiB to leave room for decoders and tasks on the N16R8 board. It retains
the whole compressed clip; it does not grow an unbounded queue or decode
the whole video into memory. The container also limits duration to ten
minutes and individual compressed records to 64 KiB.

Packets carry a separate protocol signature, session, offset, and CRC32.
Protocol version 3 uses up to 224 payload bytes per packet and 24-byte
cumulative acknowledgments. A configurable sliding window keeps multiple
packets outstanding. The receiver acknowledges after eight packets or a
short timer, and reports gaps without appending duplicates. The sender
retries from the acknowledged offset after a 30 ms data timeout. Begin
and finish messages retry after 250 ms; failed ACK submissions are retained
for another attempt. Both boards still fit ESP-NOW v1's packet-size limit.
Complete records are separately checked before being published to playback.
The final whole-file CRC runs incrementally during reception. An invalid
record or a 10-second receive timeout stops playback and discards the partial
RAM clip. CRC detects corruption, not authentication; ESP-NOW is unencrypted.

The sender preloads the compressed clip into PSRAM so SD reads do not
interrupt packet transmission. A dedicated radio task handles packet
validation, receive-buffer publication, acknowledgments, and retransmission.
Wi-Fi callbacks only queue received packets or signal send completion.
The main loop owns player setup/cleanup, decodes H.264, converts I420 to
RGB565, and draws the LCD. A separate task decodes AAC and feeds PDM audio.
At new-transfer/error boundaries the radio task waits for the main loop
to stop playback before replacing its RAM. During data transfer it runs
independently of LCD drawing. CRC uses a portable nibble lookup table.
The video clock follows submitted audio samples, allowing for DMA buffering.
Late video frames are still decoded to retain H.264 references, but their
LCD draw may be skipped. Audio and video timing can still need hardware
calibration. Decoder inputs are copied to scratch buffers so writable
decoder APIs cannot alter retained stream data.

Pinned vendor libraries, source revisions, and licenses are recorded in
[lib/esp_stream_codecs/README.md](lib/esp_stream_codecs/README.md).

## Validation

The optimized `sender` and `receiver` builds passed. All three C++ host
test suites passed with AddressSanitizer and UndefinedBehaviorSanitizer,
and all four Python uploader tests passed. The generated H.264 and AAC
streams previously passed FFmpeg decoding on the Mac. The optimized
firmware still needs the manual uploads and hardware benchmarks above.

From the parent folder:

```sh
c++ -std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -I wireless_stream/include \
    wireless_stream/tests/stream_test.cpp -o /tmp/wireless_stream_test
/tmp/wireless_stream_test media/clip.lcdstream

c++ -std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -I wireless_stream/tests/host \
    -I wireless_stream/include wireless_stream/tests/ram_clip_test.cpp \
    wireless_stream/src/ram_clip.cpp -o /tmp/wireless_stream_ram_test
/tmp/wireless_stream_ram_test media/clip.lcdstream

c++ -std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -I wireless_stream/include \
    wireless_stream/tests/window_test.cpp -o /tmp/wireless_stream_window_test
/tmp/wireless_stream_window_test

pio run -d wireless_stream -e sender -e receiver \
    -e sender-usb -e receiver-usb

python3 \
    wireless_stream/tests/test_upload.py
```

Host tests cover fragmented input, early playback buffering, malformed
headers, every record's corrupted payload, packet corruption, stale ACKs,
duplicate append rejection, final CRC, retained replay, and clearing RAM.
Window tests simulate lost data and ACKs, delayed duplicate ACKs, invalid
offsets, partial final packets, and window sizes 1 through 64. The faster
CRC is checked against a bitwise reference and a standard known vector.
Uploader tests simulate serial responses to check that the exact smaller
file is copied, old/wrong-role firmware is rejected before `put`, and a bad
chunk acknowledgement stops copying.
These tests do not execute the Xtensa decoder binaries. Two-board playback,
LCD colors/cropping, speaker output, buffering behavior, and A/V sync need
hardware testing after flashing.
