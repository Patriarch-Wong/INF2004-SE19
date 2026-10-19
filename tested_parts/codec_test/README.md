# ESP32-S3 Opus encode/decode test

Standalone codec smoke test for the ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM).
No microphone, speaker, SD card, Wi-Fi, or second ESP is needed.

## What it does

At boot, the ESP generates three seconds of mono PCM16 audio: two tones, a
frequency sweep, and a final half-second of silence. It encodes the entire clip
into Opus packets in PSRAM, destroys the encoder, creates a fresh decoder, and
decodes the stored packets in order. It pads the input to flush encoder delay,
then removes that delay and the padding before comparing the audio.

Defaults in `include/app_config.h`:

| Setting | Value |
| --- | --- |
| PCM sample rate | 16,000 Hz |
| Channels / sample format | Mono / signed 16-bit |
| Opus application | Audio |
| Bitrate | 24,000 bit/s, CBR |
| Frame duration | 20 ms / 320 samples |
| Encoder complexity | 5 |
| Test duration | 3 seconds |

Serial output reports packet sizes, compressed size, sample counts, correlation,
SNR, average/worst encode and decode times, frames exceeding 20 ms, available
memory, and task stack headroom. PASS requires all codec calls and packet/sample
checks to succeed, correlation above 0.8, and SNR above 6 dB. These are smoke-test
thresholds for the generated signal, not a perceptual quality assessment. Opus
is lossy, so decoded PCM is not expected to match the original byte for byte.

Timing covers codec calls only; it excludes test-signal generation, comparison,
serial output, and scheduler yields. Exceeding 20 ms is reported separately from
correctness: an offline file conversion may still work even if it is too slow
for live streaming.

## Build, flash, and run

Run these commands from the `codec_test/` directory (or open that directory
in PlatformIO). The connected USB-to-UART bridge uses the default environment:

```sh
pio run
pio run -t upload --upload-port PORT
pio device monitor --port PORT
```

Serial baud is 115200. The test runs automatically at boot. Send `r` to run it
again; in a line-buffered terminal, press Enter after `r`. A request during a
running test queues a repeat. Use `pio device list` if the port name changes.

For the chip's native USB connector, use `-e esp32-s3-n16r8-usb` for build,
upload, and monitor. That alternative is supported by the project configuration;
the UART environment is the one used for the connected board test.

An optional automated serial check records a transcript and exits nonzero on
FAIL or timeout. Run it with a Python interpreter that has `pyserial` installed
(PlatformIO's Python already includes it):

```sh
python scripts/check_serial.py --port PORT --runs 3 --output .pio/opus-test.log
```

Close other serial monitors before running this check or uploading firmware.

### Measured on the connected ESP32-S3

Three consecutive runs passed at the defaults above:

| Measurement | Result |
| --- | --- |
| Original PCM | 96,000 bytes |
| Opus payload / packets | 9,060 bytes / 151 packets |
| Stream including length prefixes | 9,362 bytes (10.25x smaller) |
| Encode, average / worst observed | 6.346 ms / 7.021 ms |
| Decode, average / worst observed | 2.385 ms / 2.675 ms |
| Codec calls exceeding 20 ms | 0 |
| Delay-aligned correlation / SNR | 0.9991 / 27.26 dB |
| Free heap after cleanup | 306,548 bytes, stable over the three runs |
| Free PSRAM after cleanup | 8,326,019 bytes, stable over the three runs |

These measurements use the generated signal with the radio off. The serial
transcript is included in [results/opus-roundtrip-uart.txt](results/opus-roundtrip-uart.txt).
The capture starts during the first run; the next two runs are recorded in full.
New captures default to a user-selected path such as `.pio/opus-test.log`.

## How this leads into ESP-NOW file transfer

This version tests encode/decode locally. It does not send or receive ESP-NOW
packets, read a real input file, or create a standard `.opus`/Ogg file.

The in-memory stream repeats `[uint16 little-endian packet length][Opus packet]`.
At these defaults, a packet should contain 60 bytes of Opus payload. Packet
boundaries must survive any later file transfer because raw Opus packets are
not self-delimiting. The sample rate, channel count, encoder lookahead, and
original sample count currently come from the local test; a transferable file
format will also need to preserve that metadata. A standard Ogg Opus file needs
an Ogg container reader/writer in addition to the raw codec.

ESP-NOW transfer reliability, ordering/reassembly, and radio throughput remain
untested. Opus compresses audio, not arbitrary binary files.

## Memory and library

Large PCM and packet buffers live in PSRAM. Encoder and decoder states use
internal RAM and are allocated sequentially. One dedicated task runs all Opus
calls with a 64 KiB task stack. The pinned library also uses a shared 60,000-byte
heap scratch area (`NONTHREADSAFE_PSEUDOSTACK`), so do not call this library from
multiple tasks concurrently. That scratch allocation can remain after the first
run; repeated runs should have stable memory after cleanup.

`platformio.ini` pins the Arduino library to commit
`bae0f8570b03071d0101445059e7178bed8bd144`. Its package version is 1.3.2 and the
bundled codec reports its own version at runtime. This is the archived Arduino
port used for this bounded test, not a claim to use the latest upstream Opus.

The existing `qio_opi` settings and custom 16 MB partition table are retained.
Startup reports detected flash and PSRAM sizes and warns on a mismatch.

- [Arduino libopus source](https://github.com/pschatzmann/arduino-libopus)
- [Opus encoder API](https://opus-codec.org/docs/opus_api-1.5/group__opus__encoder.html)
- [Opus decoder API](https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html)
