# Encrypted camera video over ESP-NOW

Two boards:

| Board | Hardware | Environment |
| --- | --- | --- |
| 1, camera | ESP32-S3 N16R8 CAM with the OV5640 (the `espcam_ov5640_test` board) | `camera` (UART USB-C) or `camera-usb` (native USB) |
| 2, display | ESP32-S3 N16R8 with the ST7789 240 × 240 LCD, handset wiring | `display` |

Board 1 captures small JPEG frames (240 × 240 by default, JPEG quality 30),
splits each into chunks and seals every chunk with AES-128-GCM under a
session key. Board 2 opens the chunks, rebuilds the frame, decodes it and
draws it on the LCD.

Status: **first hardware run 2026-11-06 worked but was slow (about 6 fps)
and froze once; fixes below are built but not yet flashed.**

## How it is protected

The same stack as the voice projects (files copied unchanged from
`handset_integration_test`):

- Keys come from `../shared_keys/e2e_keys.h`: the 1-2 pair key and the
  network key. No pairing step.
- Every boot, board 1 starts a handshake (`session.h`). Both sides add a
  fresh random nonce, and the session key is derived from the pair key and
  both nonces. Only a holder of the 1-2 pair key can complete it.
- Every chunk is a `secure_message`: AES-128-GCM with the header as
  associated data, plus a network-key hop tag. A per-session counter and a
  64-entry replay window reject replayed chunks. Frames recorded before a
  reboot fail under the new session key.
- Nothing about the picture goes over the air unencrypted. Only the
  8-byte header (version, type, IDs, counter) is in the clear.

Boards in use (both MACs pinned in `include/network_config.h`):

| Board | Port | MAC |
| --- | --- | --- |
| 1, camera | `/dev/cu.usbmodem5B420192791` | `E0:72:A1:D7:F6:50` (board 3 in the e2e tests) |
| 2, display | `/dev/cu.usbmodem5B420192911` | `E0:72:A1:D8:F6:20` (board 2 of `mic_speaker_e2e_test`) |

The firmware does not need the MACs. Board 1 sends its hello to the
broadcast address, and board 2 broadcasts its reply. Each board then learns
the other's MAC only once the other has proved it holds the pair key: board 1
from the reply's tag, board 2 from the confirm or the first frame under the
new key. After that, video goes unicast, so it gets MAC-layer
acknowledgements and retries. To also drop frames from other radios using the
same ID, the MACs are pinned in `include/network_config.h` (all zero there
turns the check off). A board whose own MAC differs from its pin refuses to
start.

## Frame format and size

```text
ESP-NOW frame (≤ 250 B) = 8 header + 210 sealed + 16 GCM tag + 16 hop tag
sealed plaintext        = frame ID (2) | chunk index (1) | chunk count (1) | capture ms (4) | 202 JPEG bytes
```

Lost chunks are not resent. A frame missing any chunk is dropped and the
next one replaces it.

**Playback buffer.** Board 2 doesn't draw frames the moment they complete.
It keeps them in a 12-slot buffer and shows each one at its capture time
plus a fixed delay (300 ms by default; `d` cycles 150, 300, 500 and 800 ms).
Uneven radio arrival then doesn't show as stutter; the cost is that delay.
If board 2 falls behind, it skips to the newest frame that is due. A camera
restart or a stall over 1 s re-anchors the clock. Board 2 reports frames shown,
incomplete frames and rejects to board 1 once a second, sealed the same way.
If board 1 hears no report for 3 s (board 2 reset or out of range), it
starts a new handshake.

To keep frames small:

- Default 240 × 240 at JPEG quality 12. On the camera board's serial
  monitor, `-` raises the quality number (smaller, blurrier frames), and `+`
  lowers it (better picture).
- Smaller sizes: `1` 96×96, `2` 160×120, `3` 240×176, `4` 240×240. Smaller
  images are centred on the LCD.
- The camera's stats line shows bytes per frame and chunks per frame.

Board 1 sends at most 12 frames a second (`max_fps`), about 30 KB/s at
quality 12. It keeps up to 6 chunks in flight in ESP-NOW instead of waiting
for each one's "sent" callback. Board 2 decodes a
frame in about 38 ms and draws it in about 25 ms at 40 MHz SPI. If the
picture shows noise or shifted pixels at 40 MHz, set `lcd_spi_hz` back to
10 MHz (about 100 ms per frame, so about 6 fps).

## Build, flash, run

```sh
cd tested_parts/espcam_espnow_video_test
pio run -e camera  -t upload --upload-port /dev/cu.usbmodem5B420192791  # board 1
pio run -e display -t upload --upload-port /dev/cu.usbmodem5B420192911  # board 2
pio device monitor -e camera  --port /dev/cu.usbmodem5B420192791
pio device monitor -e display --port /dev/cu.usbmodem5B420192911
```

Hold BOOT, tap RESET, release BOOT before each upload, then press RESET.
Either board can start first.

Expected on board 1:

```text
Encrypted ESP-NOW camera link | board 1 (camera), peer board 2
KEYS | pair 1-2 key XXXXXXXX (must match on both boards)
Wi-Fi station MAC: ...
RADIO channel 1 rate 1 Mbps, power save off
CAMERA OV5640 | 240x240 | JPEG quality 30
SESSION hello: looking for board 2
SESSION established | session XXXXXXXX
PEER board 2 at ...
TX 8.0 fps | frame ... B avg (...) | ... chunks | ... KB/s | ack fail 0 | 240x240 q30 | display shown ... incomplete 0 rejected 0
```

Expected on board 2: `SESSION replied`, then `SESSION confirmed`, then
`RX ... fps | 240x240 | ...` lines and the picture on the LCD. Before the
camera connects, the LCD shows "Waiting for camera" and board 2's MAC.

| Command | Board | Effect |
| --- | --- | --- |
| `+` / `-` | 1 | Better picture / smaller frames (JPEG quality ∓2) |
| `1`–`4` | 1 | 96×96, 160×120, 240×176, 240×240 |
| `s` | both | Session, peer and counters |
| `b` | 2 | Swap the pixel byte order (fixes wrong, "fried" colours) |
| `i` | 2 | Toggle display inversion (negative image) |
| `f` | 2 | LCD SPI 40 → 20 → 10 MHz (noise or shifted pixels) |
| `d` | 2 | Playback delay 150 → 300 → 500 → 800 ms |

## Host tests

```sh
tests/run_host_tests.sh      # needs brew install openssl@3
```

- `secure_message` and `session`: the suites from `handset_integration_test`.
- `video_link`, the camera-to-display path:
  - handshake, then a 4,321-byte 240×240 JPEG sealed as 22 chunks, delivered
    out of order, rebuilt byte for byte
  - replayed chunk rejected, tampered chunk rejected
  - a frame with a lost chunk counted incomplete when the next frame starts;
    a late chunk from it marked stale
  - wrong pair key fails the handshake
  - after a display reboot, old frames get `no_session` and then fail under
    the new key
  - chunk size limits and the status report
  - camera restart: frame IDs starting again at 1 are accepted, not stale
  - playback buffer: delay, capture-time spacing, skipping when behind,
    overflow, and re-anchoring after a camera restart or a stall

## Pass criteria (hardware)

- Both boards print the same `pair 1-2 key` fingerprint, then the same
  session ID.
- The LCD shows live video with correct colours. If colours are wrong, set
  `pixels_big_endian = false` in `include/app_config.h`.
- Board 1: `ack fail` near 0. Board 2: `incomplete` and `rejected` stay low
  with the boards a few metres apart.
- Reset board 2 mid-stream: video returns within about 3 s with a new session
  ID.
- Record the frame size and fps at 240×240 for a few quality settings.

## Hardware results

### Run 1, 2026-11-06 (boards 1 and 2 on the desk)

Settings: 240×240, JPEG quality 30, ESP-NOW unicast, channel 11, 1 Mbps,
LCD at 10 MHz, `max_fps` 8.

- Keys and session matched on both boards (`pair 1-2 key E14ACBA8`).
- Frames were 2.6–2.75 KB (13–14 chunks). The radio carried 15–19 KB/s,
  0 incomplete frames, 0 queue drops.
- **5.7–6.7 fps, set by the display.** Decode took 38 ms and drawing 97–102 ms
  per frame at 10 MHz SPI, about 140 ms in all. The 100 ms draw is also a
  visible top-to-bottom wipe.
- **One freeze and a new session about 3 s in.** Board 2 handled every
  received frame before doing anything else. Running near its limit, it
  rarely got round to its status report. Board 1 heard nothing for 3 s,
  treated the link as lost and ran a new handshake.
- Board 1 counted 11–18 chunks per 2 s without an acknowledgement (about 9%),
  but board 2 lost no frames, so those chunks did arrive; only the
  acknowledgements were missed. Board 2's "rejected" count (8/s) mixed
  frames from three other handsets on channel 11 (wrong network key) with
  real rejects.

Changes after run 1: channel 11 → 1 (see below); LCD 10 → 40 MHz; `max_fps` 8 → 12; board 2 handles at
most 8 received frames per loop pass and reports every 500 ms. Stats now
split other boards' traffic from real rejects (replay counted separately)
and split missed acknowledgements from send timeouts.

### Channel move, 2026-11-06

A second capture found up to 80 frames/s on channel 11 from three handsets
running other projects (`E0:72:A1:D7:FA:B8`, `E0:72:A1:D9:A4:94`,
`E0:72:A1:D8:E2:10`). They cannot open our frames, but they share the air
time. The Mac's Wi-Fi scan showed 3 networks on channel 11, 2 on channel 6
and none on channel 1, so this project now uses channel 1. Both boards must
run a build with the same channel.

### Run 2, 2026-11-06 (channel 1, LCD 40 MHz, `max_fps` 12, quality 30)

- 0 frames from other boards on channel 1.
- LCD at 40 MHz worked: draw 27–30 ms, decode 38–39 ms.
- Sent 6.7–8.4 fps of 2.7 KB frames (18–22 KB/s). Shown: 4.5–6.0 fps.
  About 2 frames/s were incomplete, with 0 rejects and 0 queue drops, so the
  chunks never arrived.
- Every failed send was a **timeout** (30–40 a second), never a missed
  acknowledgement. The camera task shares core 0 and priority 23 with Wi-Fi.
  Capturing continuously, it delayed the radio's "sent" callbacks past
  30 ms, and the camera moved on early, losing chunks.
- On screen: **wrong, "fried" colours and a blocky picture.** Fried colours
  are swapped RGB565 bytes; quality 30 is very coarse.

Changes after run 2:
- pixel byte order flipped (`pixels_big_endian = false`, `b` toggles it)
- camera captures one frame at a time (`fb_count` 1, `GRAB_WHEN_EMPTY`)
- send wait 30 → 100 ms, with `esp_now_send` retried when the driver is busy
- JPEG quality 30 → 12 and `max_fps` 12 → 5, favouring picture over frame rate
- `i` and `f` added on board 2 to test inversion and SPI speed live

### Run 3, 2026-11-06 (quality 12, `max_fps` 5, colours fixed)

- No lost frames, no rejects, and 3 send timeouts in 20 s.
- Only 2.9–3.9 fps reached the screen (cap 5). A live quality sweep over
  serial showed that bigger frames meant fewer fps:

  | Quality | Frame | Chunks | fps |
  | --- | --- | --- | --- |
  | 60 | 1.87 KB | 10 | ~4.2 |
  | 30 | 2.0 KB | 10 | ~4.3 |
  | 12 | 2.57 KB | 13 | ~3.5 |
  | 4 | 3.65 KB | 18 | ~2.2 |

  That is 15–30 ms per chunk, against about 3 ms of air time. The camera
  waited for each chunk's "sent" callback before sending the next.
- **Bug: the display froze when the camera restarted.** The new frame IDs
  (from 1) looked older than the last frame shown, so every chunk was
  "stale". Opening the camera's serial port restarts it, which is how this
  showed up.

Changes after run 3:
- chunks pipelined, up to 6 in flight
- two camera buffers, so the next capture overlaps sending
- capture time in every chunk, and the playback buffer on board 2
- frame IDs far behind start a new frame; a new session clears the buffers
- `max_fps` 5 → 12
- capture and send times per frame added to the camera's stats line
