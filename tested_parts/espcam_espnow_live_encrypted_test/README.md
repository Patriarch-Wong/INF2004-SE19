# Encrypted live camera over ESP-NOW

[`espcam_espnow_live_test`](../espcam_espnow_live_test/) with every packet
encrypted. The video path is unchanged: the newest 160×120 frame, 6 Mbps
unicast, no playback buffer, scaled up on the 240×240 LCD. Each packet is
now sealed with the same session-key stack as
[`espcam_espnow_video_test`](../espcam_espnow_video_test/) and the voice
projects.

| Board | Hardware | Port | MAC | Environment |
| --- | --- | --- | --- | --- |
| 1, camera | ESP32-S3 N16R8 CAM, OV5640 | `/dev/cu.usbmodem5B420192791` | `E0:72:A1:D7:F6:50` | `camera` (UART) / `camera-usb` |
| 2, display | ESP32-S3 N16R8, ST7789 240 × 240 | `/dev/cu.usbmodem5B420192911` | `E0:72:A1:D8:F6:20` | `display` |

Status: **builds and host tests pass; not yet run on hardware.**

## How it is protected

- Keys come from `../shared_keys/e2e_keys.h`: the 1-2 pair key and the
  network key. No pairing step.
- Every boot, board 1 runs a handshake with board 2 (`session.h`). Each side
  adds a fresh random nonce, and the session key is derived from the pair
  key and both nonces. Only a holder of the 1-2 pair key can complete it.
- Every video packet and status report is a `secure_message`: AES-128-GCM
  with the 8-byte header as associated data, plus a network-key hop tag. A
  per-session counter and a 64-entry replay window reject replays. Packets
  recorded before a reboot fail under the new session key.
- Only the 8-byte header (version, type, IDs, counter) is in the clear.
- Both MACs are pinned in `include/network_config.h`. The handshake goes
  unicast too, and frames carrying the peer's ID from any other MAC are
  rejected.
- If board 1 hears no status report for 3 s (board 2 reset or out of
  range), it runs a new handshake. Both boards then restart the frame
  sequence.

## Cost compared with the unencrypted version

```text
ESP-NOW frame (≤ 250 B) = 8 header + 210 sealed + 16 GCM tag + 16 hop tag
sealed plaintext        = 0x56 | sequence (4) | index (1) | count (1) | 203 JPEG bytes
```

- 203 JPEG bytes per packet instead of 243. A 160×120 frame of about 1 KB
  needs 5–6 packets instead of 4–5, about 20% more air time.
- Sealing and opening run on the ESP32-S3's AES and SHA hardware (mbedTLS).
  The camera's stats line shows `seal … us/packet`, and the display's shows
  `open … us/packet`.

## Build, flash, run

```sh
cd tested_parts/espcam_espnow_live_encrypted_test
pio run -e camera  -t upload --upload-port /dev/cu.usbmodem5B420192791
pio run -e display -t upload --upload-port /dev/cu.usbmodem5B420192911
```

Hold BOOT, tap RESET, release BOOT before each upload, then press RESET.
Either board can start first. Serial monitors run at 921600 baud.

Expected on board 1:

```text
Encrypted live ESP-NOW camera | board 1 (camera), peer board 2
KEYS | pair 1-2 key XXXXXXXX (must match on both boards)
RADIO channel 1 rate 6 Mbps, power save off
CAMERA OV5640 | 160x120 | JPEG quality 12 | max 15 fps
SESSION hello: looking for board 2
SESSION established | session XXXXXXXX
TX ... fps | 160x120 q12 | frame ... B, ... packets | ... | seal ... us/packet | ... | display shown ... incomplete ... rejected 0
```

Board 2 prints `SESSION replied`, then `SESSION confirmed` with the same
session ID, then `RX …` lines.

| Key | Board | Effect |
| --- | --- | --- |
| `1`–`4` | camera | 96×96, 160×120, 240×176, 240×240 |
| `+` / `-` | camera | Sharper / smaller frames (JPEG quality ∓2) |
| `r` | camera | Radio rate 1 → 2 → 6 → 12 → 24 Mbps |
| `s` | both | Session ID and counters |
| `b` | display | Swap pixel byte order (wrong colours) |
| `i` | display | Toggle inversion (negative image) |
| `f` | display | LCD SPI 40 → 20 → 10 MHz (noise, shifted pixels) |

## Pass criteria (hardware)

- Both boards print the same `pair 1-2 key` fingerprint, then the same
  session ID.
- The picture looks as smooth as `espcam_espnow_live_test` at the defaults.
  Compare the fps on both stats lines with that project's.
- `rejected` stays 0 on both boards. `other frames` counts other projects'
  boards on channel 1 and is not an error.
- Reset board 2 mid-stream: video returns within about 3 s with a new
  session ID. Reset board 1: same.

## Host tests

```sh
tests/run_host_tests.sh      # needs brew install openssl@3
```

- `secure_message` and `session`: the suites from `handset_integration_test`.
- `video_link`:
  - packet sizes (203 JPEG bytes, 210-byte packets), a frame rebuilt byte
    for byte out of order and with duplicates
  - an abandoned frame counted, a late packet marked stale, a camera
    restart (sequence 1 after 300), receiver reset on a new session
  - sealed path: handshake, a 1,100-byte frame as 6 sealed 250-byte packets
    with no picture bytes in the clear, opened out of order and rebuilt;
    replay rejected; tampered packet fails the hop tag, and with the hop tag
    re-signed it fails the end-to-end tag; after a camera reboot, old-session
    packets are rejected and the new session's frame 1 is shown; a wrong pair
    key cannot complete the handshake
  - malformed packets, 160×120 → 240×180 and 96×96 → 240×240 scaling, the
    JPEG size reader
