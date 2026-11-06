# Live camera over ESP-NOW (no encryption)

A from-scratch redo of [`espcam_espnow_video_test`](../espcam_espnow_video_test/),
which played at about 6 fps and ran roughly 0.7–0.9 s behind live. This
version is built for low lag and has **no encryption**.

| Board | Hardware | Port | MAC | Environment |
| --- | --- | --- | --- | --- |
| 1, camera | ESP32-S3 N16R8 CAM, OV5640 | `/dev/cu.usbmodem5B420192791` | `E0:72:A1:D7:F6:50` | `camera` (UART) / `camera-usb` |
| 2, display | ESP32-S3 N16R8, ST7789 240 × 240 | `/dev/cu.usbmodem5B420192911` | `E0:72:A1:D8:F6:20` | `display` |

Status: **runs on hardware (2026-11-06): the user judged the stream good
and smooth at the defaults (160×120, 6 Mbps, `max_fps` 15). No measured
fps or lag numbers were recorded.**

## What changed from the first version, and why

| | `espcam_espnow_video_test` | This project |
| --- | --- | --- |
| Capture | 240×240 | **160×120** (96×96, 240×176, 240×240 on keys), scaled up on the display |
| Frame given to the radio | older frame (`GRAB_WHEN_EMPTY`), 160–330 ms old | newest frame (`GRAB_LATEST`) |
| Radio | 1 Mbps, about 16 KB/s, 150–195 ms to send a frame | **6 Mbps** (`r` on the camera changes it live) |
| Protection | handshake + AES-GCM, 48 bytes overhead per packet | none; 7-byte header, 243 JPEG bytes per packet |
| Display | 300 ms playback buffer | newest complete frame drawn at once |
| Channel | 1 | 1 |

At 160×120 a frame is roughly 1 KB, about 4–5 packets, and the display
decodes it about 3–4 times faster than a 240×240 frame. It is scaled to
240×180 (96×96 fills the whole screen).

## How it works

- The camera grabs the newest frame (at most `max_fps`, 15) and unicasts
  it to the display's MAC in 250-byte packets, up to 8 in flight.
- The display accepts packets only from the camera's MAC and keeps only the
  newest frame. Packets from a newer frame abandon the one in progress, and
  a frame missing any packet is dropped. Nothing is resent.
- When a frame completes, the display decodes it, scales it to fit
  240×240 (nearest neighbour), and draws it straight away. If a newer
  frame completes before it is drawn, the older one is skipped.
- The display sends a 9-byte status report (frames shown, incomplete)
  twice a second, for the camera's stats line.
- A camera restart starts the sequence at 1 again; the display treats a
  sequence more than 64 frames behind as a restart, not as old packets.

Packet: `0x56` | sequence (4, LE) | index (1) | count (1) | JPEG bytes.

## Build, flash, run

```sh
cd tested_parts/espcam_espnow_live_test
pio run -e camera  -t upload --upload-port /dev/cu.usbmodem5B420192791
pio run -e display -t upload --upload-port /dev/cu.usbmodem5B420192911
```

Hold BOOT, tap RESET, release BOOT before each upload, then press RESET.
Serial monitors run at 921600 baud.

| Key | Board | Effect |
| --- | --- | --- |
| `1`–`4` | camera | 96×96, 160×120, 240×176, 240×240 |
| `+` / `-` | camera | Sharper / smaller frames (JPEG quality ∓2) |
| `r` | camera | Radio rate 1 → 2 → 6 → 12 → 24 Mbps |
| `s` | both | Counters |
| `b` | display | Swap pixel byte order (wrong colours) |
| `i` | display | Toggle inversion (negative image) |
| `f` | display | LCD SPI 40 → 20 → 10 MHz (noise, shifted pixels) |

Camera stats line: fps, frame size and packets per frame, KB/s and rate,
**frame age** (capture to send) and **send time**, packets not sent,
missed acknowledgements, and the display's shown/incomplete counts
(`SILENT` if no report for 3 s). Display stats line: fps shown, camera
and shown size, decode+scale and draw times, frames skipped, incomplete,
bad, and other boards' frames.

## Host tests

```sh
tests/run_host_tests.sh
```

These cover:
- packet sizes, and a frame rebuilt byte for byte out of order and with
  duplicates
- an abandoned frame counted, and a late packet marked stale
- a camera restart (sequence 1 after 300)
- malformed packets
- 160×120 → 240×180 and 96×96 → 240×240 scaling, and the JPEG size reader
