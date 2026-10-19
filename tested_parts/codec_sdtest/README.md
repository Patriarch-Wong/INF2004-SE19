# ESP32-S3 Opus + onboard SD card test

Combines the previous `../codec_test` Opus test with the onboard SDMMC reader
configuration from `../SDTest`. Target: ESP32-S3 N16R8, 16 MB Quad-SPI flash and
8 MB Octal-SPI PSRAM, using the existing `qio_opi` and partition settings.

## Reusable modules

- `include/codec.h` / `src/codec.cpp`: create encoder/decoder handles, encode or
  decode PCM16 frames, query lookahead, and destroy handles. The API validates
  buffer sizes and configuration; libopus types stay private.
- `include/sd_card.h` / `src/sd_card.cpp`: mount/unmount, query capacity, make
  directories, exclusively create or read files, exact-length I/O, size, and
  checked flush/close. Uses ESP-IDF's SDMMC/FAT backend directly.
- `src/codec_test.cpp`: exercises these public APIs; contains test signals,
  packet framing, timing, quality checks, and rejected-operation checks.
- `src/main.cpp`: Arduino serial and FreeRTOS task adapter.

Both public headers are usable from C99 and C++11. Initialize opaque handles to
NULL/nullptr; successful create/open calls transfer ownership to the caller.
Destroy/close clears the handle. The headers document error and threading
contracts. Implementations use C++ ownership helpers for automatic cleanup;
explicit file close still reports storage errors. No module formats the card.

See [Coding standard](../../README.md#coding-standard) for the project's embedded
subset of the C++ Core Guidelines with BARR-style formatting. This is not a
claim of full compliance with either standard.

## SD configuration

| SD signal | GPIO |
| --- | --- |
| CMD | 38 |
| CLK | 39 |
| D0 | 40 |

The reader uses **1-bit SDMMC at 20 MHz**, not SPI. Pin values and frequency are
in `include/app_config.h`. N16R8 describes memory capacity; this mapping comes
from the local SD test project, not from the module name alone.

Insert a card with a supported FAT filesystem before running. The firmware
mounts with automatic formatting disabled. Every run creates an unused
`/codec_sdtest/run_NNNN.opusbin` filename, preserving earlier files. Failed runs
may leave a partial file for diagnosis. Files are retained; remove them manually
when no longer needed. The card is unmounted before the repeat prompt, so it can
be swapped while idle. Do not remove it during a run.

## Test sequence

1. Mount the SD card and report capacity.
2. Generate three seconds of 16 kHz mono PCM16: tones, a sweep, then silence.
3. Encode 20 ms frames using Opus Audio, 24 kbit/s CBR, complexity 5.
4. Write each length-prefixed packet to SD immediately after encoding. Keep an
   independent expected copy in PSRAM for exact storage verification.
5. Flush and close the file, reopen it, and check its size and metadata.
6. Read every packet from SD, compare all bytes against the expected copy, and
   pass the SD-read packet to a fresh Opus decoder.
7. Trim encoder lookahead and padding, then compare decoded audio to the input.
8. Close the file, release test buffers, unmount the card, and report free memory.

PASS requires successful I/O and codec calls, exact file readback, correct packet
and sample counts, correlation above 0.8, and SNR above 6 dB. Opus is lossy;
byte equality applies to encoded file storage, not the decoded PCM.

Serial output reports average/worst codec and SD packet-call times, combined
encode/write and read/verify/decode times, and calls exceeding the 20 ms frame
budget. FATFS buffers sector writes; final sync/close latency is reported
separately. The SD API uses VFS file descriptors directly.
Timing excludes signal generation, header/open/mount operations, quality
comparison, serial output, and explicit scheduler yields. This is a short
functional test, not a sustained real-time recording or power-loss test.

## Build, flash, and repeat

```sh
pio run
pio run -t upload --upload-port PORT
pio device monitor --port PORT
```

Serial uses the USB-to-UART bridge at 115200 baud. The test runs once at boot;
send `r` to repeat (Enter may be needed in a line-buffered terminal). A repeat
requested while running is queued. Use `pio device list` if the port changes.
For the native USB connector, add `-e esp32-s3-n16r8-usb` to the commands.

An automated checker captures repeated runs and exits nonzero on FAIL or timeout.
Close other serial monitors first. Use Python with `pyserial` installed, such as
PlatformIO's Python:

```sh
python scripts/check_serial.py --port PORT --runs 3 --output .pio/codec-sd-test.log
```

## Test file format

This is a custom `.opusbin` container, not a standard Ogg `.opus` file. It is not
intended to open directly in a music player. The 32-byte header is:

| Offset | Encoding | Meaning |
| --- | --- | --- |
| 0 | 4 ASCII bytes | `OSD1` (format version 1) |
| 4 | uint32 LE | Sample rate in Hz |
| 8 | uint32 LE | Channel count |
| 12 | uint32 LE | Samples per frame per channel |
| 16 | uint32 LE | Original samples per channel |
| 20 | uint32 LE | Encoder lookahead in samples at the stored sample rate |
| 24 | uint32 LE | Packet count, including delay-flushing padding |
| 28 | uint32 LE | Target bitrate in bit/s |

The header is followed by repeated `[uint16 LE payload length][Opus payload]`.
This test validates metadata against the current run; it does not import arbitrary
files. Defaults produce 151 packets and a 9,394-byte file from 96,000 bytes of
original PCM. There is no microphone, speaker, or radio path in this test.

## Measured on the connected board

Both UART and native-USB firmware builds passed. The UART firmware was flashed,
and three consecutive hardware runs passed with CMD=38, CLK=39, D0=40 at 20 MHz
in 1-bit mode. The card reported 60,906 MiB capacity. Transcript:
`.pio/codec-sd-api-test.log` (local ignored build output).

| Measurement | Result across three runs |
| --- | --- |
| Exact SD readback | All 9,394 bytes matched on every run |
| Opus payload | 9,060 bytes / 151 packets |
| Correlation / SNR | 0.9991 / 27.26 dB |
| Encode + SD write, average | 6.948–6.969 ms/frame |
| Encode + SD write, worst | 14.366 ms |
| SD read + verify + decode, average / worst | 2.933–2.938 / 6.133 ms |
| Buffered SD write, worst | 7.478 ms |
| Final flush + close | 6.965–8.327 ms |
| Combined frame operations exceeding 20 ms | 0 |
| Free heap after cleanup | 302,660 bytes, stable |
| Free PSRAM after cleanup | 8,326,019 bytes, stable |

Measured files were `/codec_sdtest/run_0010.opusbin` through
`run_0012.opusbin`. Invalid-argument, file-mode, duplicate-file preservation,
and busy-unmount checks also passed. The native USB environment was retained but was not used for
these hardware checks.

## Memory and dependencies

PCM and expected packet buffers use PSRAM. Encoder and decoder states use
internal RAM sequentially. A dedicated task provides a 64 KiB stack. The pinned
Arduino libopus port uses shared scratch memory; keep codec calls in this task.
Its retained scratch allocation can make the first run's memory use differ from
startup; subsequent post-cleanup values should stabilize.

PlatformIO Espressif32 is pinned to `7.1.3`; libopus is pinned to commit
`bae0f8570b03071d0101445059e7178bed8bd144`, matching the previous codec test.

- [Arduino-ESP32 2.0.17 SDMMC API](https://github.com/espressif/arduino-esp32/blob/2.0.17/libraries/SD_MMC/src/SD_MMC.h)
- [Opus decoder API](https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html)
- [Arduino libopus source](https://github.com/pschatzmann/arduino-libopus)
