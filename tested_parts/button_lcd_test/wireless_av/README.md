# ESP-NOW SD video and audio player

For the smaller H.264/AAC format and playback while receiving into RAM,
see [wireless_stream](../wireless_stream/README.md). That variant needs an
SD card only on the sender; this project's behavior remains as below.

Two ESP32-S3 N16R8 builds transfer a complete clip to the receiving board's
SD card, verify the saved file, then play video and audio together there.
This is file transfer followed by playback, not a live radio stream.
The original single-board LCD player remains in the parent folder.

| Role | Station MAC | PlatformIO environment |
| --- | --- | --- |
| Sender | E0:72:A1:D7:F6:50 | `sender` |
| Receiver | E0:72:A1:D9:A4:94 | `receiver` |

Each build checks its own station MAC and refuses to initialize ESP-NOW
on the wrong board. Both use Wi-Fi channel 1. MACs and pins are in
[`include/app_config.h`](include/app_config.h).

Commands below use `SENDER_PORT` and `RECEIVER_PORT`. Find them with
`pio device list`; connect one board at a time to tell them apart. Each
firmware prints its station MAC at startup, and `esptool.py read_mac`
also reports it.

## Upload and send flow

```text
Flash sender firmware and receiver firmware
                    |
Copy media/clip.lcdav from Mac to sender SD over USB
                    |
Press sender GPIO4 button (or enter serial command: send)
                    |
Sender SD -> ESP-NOW -> receiver SD
                    |
Receiver verifies saved file -> plays video and audio
```

Follow the commands below in order. Firmware upload and USB clip copying
are setup steps; later sends only need both boards powered and a press of
the sender button.

## Receiver wiring

| Signal | GPIO |
| --- | --- |
| LCD RES | 10 |
| LCD SDA / MOSI | 11 |
| LCD SCL / SCK | 12 |
| LCD DC | 9 |
| LCD BLK | 14, driven high |
| Speaker signal | 8 |
| Built-in SD CMD | 38 |
| Built-in SD CLK | 39 |
| Built-in SD D0 | 40 |
| Optional pause/resume button | 4 to GND |
| Optional LED | 5 through a series resistor |

The receiver assumes the same 240 x 240 ST7789 without CS. It uses
SPI mode 3 at 10 MHz. Backlight GPIO14 is a power/enable signal only as
supported by your LCD breakout; a raw backlight LED needs a driver.
The built-in SD card uses 1-bit SDMMC at 20 MHz on both boards.
Use a FAT-formatted card; neither build formats cards.

Audio follows the PDM approach used in your
[speaker project](https://github.com/Patriarch-Wong/esp32_tests/tree/main/speaker_test),
with the signal moved to GPIO8. It requires the same compatible amplified
speaker input and common ground. GPIO8 is not a power driver for a bare
speaker. `audio_gain_percent` defaults to 15; this does not configure a
three-wire I2S DAC, which would require BCLK/LRCLK/DATA pins instead.

## Prepare both SD cards on macOS

Both boards need an SD card. The sender stores `/clip.lcdav`; the receiver
saves `/received.lcdav` and reads the video from it during playback.
The receiver saves its copy automatically after the wireless transfer.

Formatting erases all files on the selected card. Back up anything you
want to keep first. For a typical 4-32 GB microSD card:

1. Connect the card to your Mac using a card reader.
2. Open Disk Utility and choose **View -> Show All Devices**.
3. Select the SD card's top-level device. Check its capacity to make sure
   you selected the correct device.
4. Click **Erase**. Set the name to `ESP32SD`, format to **MS-DOS (FAT)**,
   and scheme to **Master Boot Record**.
5. Click **Erase**, then **Done**. Select the resulting volume and open
   **Info** to confirm the filesystem is **MS-DOS FAT32**.
6. Eject the card, insert it into the powered-off board, then power on.
7. Repeat for the other board's card.

These steps target 4-32 GB cards; larger cards may need a different FAT32
formatting procedure. See Apple's
[Disk Utility instructions](https://support.apple.com/en-qa/guide/disk-utility/dskutl1010/mac)
and SanDisk's
[SD-card formatting guide](https://support-en.sandisk.com/app/answers/detailweb/a_id/35101).

## Flash the two boards

Run all commands below from the parent `button_lcd_test` folder:

```sh
cd tested_parts/button_lcd_test
pio device list
```

Check that the listed ports match the commands below. If opening
`wireless_av` itself as a PlatformIO project, omit `-d wireless_av` from
the upload commands.
Close serial monitors. Put the appropriate board in download mode by
holding BOOT, tapping RESET, and releasing BOOT if needed.

Sender:

```sh
pio run -d wireless_av -e sender -t upload \
    --upload-port SENDER_PORT
```

Receiver:

```sh
pio run -d wireless_av -e receiver -t upload \
    --upload-port RECEIVER_PORT
```

Press RESET after flashing each board. The receiver should show
`Waiting for clip`. Use `pio device list` to find ports. For a native USB
connection instead of the UART bridge, use `sender-usb` / `receiver-usb`.
All serial commands run at 921600 baud with DTR/RTS disabled.

## Copy the clip and transmit it

The prepared file is `../media/clip.lcdav`: 4,615,086 bytes (4.40 MiB).
It contains all 911 video frames at 240 x 136 / 12 fps and 75.917 seconds
of 16 kHz mono G.711 mu-law audio. The earlier `video.lcdv` is silent;
use the new `clip.lcdav` bundle for this project. The bundle is not
committed to Git; build it first with
[Regenerate the bundle](#regenerate-the-bundle).

With BOTH new firmwares running and both cards inserted, close serial
monitors and run this from the parent `button_lcd_test` folder:

```sh
python3 \
    wireless_av/tools/send_clip.py \
    --port SENDER_PORT media/clip.lcdav
```

Or use any Python environment with `pyserial` installed. The script:

1. Copies the bundle to the sender's `/clip.lcdav` over USB in acknowledged
   4096-byte chunks and checks SHA-256 by reading it back from SD.
2. Exits with the clip ready on the sender's SD card.

Wait for these messages before sending:

```text
AV clip verified on sender SD.
Press the sender's GPIO4 button to send it over ESP-NOW.
```

Press the sender's button (GPIO4 to GND) to start the ESP-NOW transfer.
A press must remain stable for 30 ms. Holding it does not repeat sending;
release and press again for another transfer. A button held at startup
must first be released. Button presses during an active transfer are ignored.
The clip stays on SD, so the Mac is not needed for later button-triggered
transfers. Booting the sender or copying the file does not start a transfer.

The receiver writes, closes, reopens, and CRC32-checks its SD file, then
starts playback automatically. Sender status `SENT VERIFIED` confirms the
saved file, not physical A/V output. Transfer may take several minutes.
There is no need to insert either SD card in the Mac. The serial `send`
command remains available for diagnostics. After the copy script exits,
open the sender monitor to watch progress or trigger a send:

```sh
pio device monitor --port SENDER_PORT \
    --baud 921600 --dtr 0 --rts 0
```

Type `send` and press Enter to start, or `status` and Enter for progress.
A successful transfer of the prepared clip reports
`SENT VERIFIED 4615086 bytes CRC32 ...`. Exit the monitor with Ctrl+C
before another firmware upload or USB clip copy.

The receiver replays `/received.lcdav` automatically after reboot. Its
GPIO4 button pauses/resumes both video and audio. The sender has no local
LCD/audio playback in this separate project.

Existing matching files are verified and reused. Different existing final
files are preserved and cause an error. To replace the clip later, rename
or remove `/clip.lcdav` on the sender and `/received.lcdav` on the receiver
first. Failed radio transfers may leave uniquely named `/av_*.part` files;
they are never selected for playback. Unrelated card files are untouched.

## Playback and transfer design

The receiver loads about 1.16 MiB of mu-law audio into PSRAM, decodes it
in a dedicated task, and feeds the I2S PDM transmitter. Video reads from SD
and follows the audio sample clock, accounting for queued DMA samples.
Overdue video frames are skipped. A/V sync, speaker quality, and sustainable
frame rate still need testing on the receiver; DMA buffering gives timing
a granularity of about 16 ms and may require hardware latency calibration.

ESP-NOW packets contain 224 data bytes and a 24-byte header. Each packet
has CRC32, session ID, byte offset, and an application acknowledgement.
Duplicate packets are acknowledged without a second write. Retries occur
after 150 ms, up to 100 attempts. SD I/O runs in the main loop, outside the
radio callback. A completed file gets a separate full read-back CRC check.
This follows the small-packet and application-ACK guidance in
[Espressif's ESP-NOW documentation](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-reference/network/esp_now.html).
The MAC filters are not cryptographic authentication; packets are unencrypted.

## Read the serial monitors

Open the receiver monitor in a terminal:

```sh
pio device monitor --port RECEIVER_PORT \
    --baud 921600 --dtr 0 --rts 0
```

Press RESET on the receiver to see startup messages when no transfer is
in progress. Type `status` and press Enter to request its current state.
Your typing may not appear on screen; press Enter to submit the command.

To watch the sender at the same time, open a second terminal:

```sh
pio device monitor --port SENDER_PORT \
    --baud 921600 --dtr 0 --rts 0
```

Enter one command per line:

| Board | Command | Action |
| --- | --- | --- |
| Either | `status` | Report SD/radio state and progress |
| Sender | `send` | Start sending the saved clip over ESP-NOW |
| Receiver | `play` | Restart the saved clip |
| Receiver | `pause` | Toggle video/audio pause and resume |

Read the output as follows (`...` represents changing values):

| Message | Meaning |
| --- | --- |
| `SD READY` | SD card mounted successfully |
| `SENDING ...` | Sender started a transfer |
| `ESP-NOW ...` | Sender transfer progress in bytes |
| `SENT VERIFIED ...` | Receiver confirmed the saved file |
| `RECEIVED VERIFIED ...` | Receiver's saved file passed verification |
| `PLAYING AV ...` | Receiver started video/audio playback |
| `AV LOOP ...` | Playback started another loop |
| `ERROR ...` | An operation failed; read the accompanying description |

`SENT VERIFIED` confirms file transfer; check the receiver's playback logs
and physical LCD/speaker output to confirm playback.

Press Ctrl+C to close a monitor. Close the relevant board's monitor before
uploading firmware or copying the clip over USB. If a port is missing,
run `pio device list` again and update the command with its current port.

## Diagnostics and validation

Serial `status` shows SD/radio state, progress, queue drops, and receiver
playback counters. Receiver commands `play` and `pause` restart or toggle
playback. A missing receiver/channel mismatch causes a sender timeout.
A checksum/SD/player error is reported to serial; receiver errors also
appear on the LCD. File-transfer success and playback status are separate.

Host checks:

```sh
c++ -std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -I wireless_av/include wireless_av/tests/protocol_test.cpp \
    -o /tmp/wireless_av_protocol_test
/tmp/wireless_av_protocol_test
```

Tests cover a known CRC32 vector, all 1,984 single-bit packet corruptions,
stale ACK rejection, and malformed/overflowing bundle headers. All 256
mu-law code values also matched FFmpeg's PCM decoding. The bundled video
matches the previously validated 911-frame video byte for byte.

All four builds passed: sender/receiver with UART and native USB. The new
wireless A/V firmware has not yet been tested on both boards together;
receiver display output, audio output, and A/V synchronization remain
hardware checks after flashing.

## Regenerate the bundle

From the parent folder after generating `media/video.lcdv` as in the
[parent README](../README.md#generate-the-video):

```sh
python3 tools/package_av.py media/source.mp4
```

The audio argument can be any file with an audio track; only its audio
is used.

`tools/package_av.py` uses FFmpeg to encode 16 kHz mono mu-law audio,
padding/trimming to the exact video duration. A 32-byte `LCDAV001` header
stores header size, video offset, audio offset, audio sample count,
sample rate, and video byte count as six little-endian 32-bit integers.
Raw mu-law audio follows, then the original `LCDV0001` video container.
