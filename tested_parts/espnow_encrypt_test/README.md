# ESP-NOW encrypted whitelist test

This is [espnow_test](../espnow_test/) with ESP-NOW encryption turned on.
Two ESP32-S3 N16R8 boards exchange numbered packets once per second on Wi-Fi
channel 1. The peer is added with `encrypt = true` and a shared local master
key (LMK), so frames are protected with CCMP (AES-128). Without the key, nobody
can read the payload or forge or modify a frame. The receiving Wi-Fi driver
drops frames that fail the CCMP integrity check before `onReceive` is called.

Status: hardware-tested 2026-10-26; all four steps passed. See
[Test results](#test-results).

## Keys

| Key | Purpose |
| --- | --- |
| PMK (primary master key) | Set with `esp_now_set_pmk()`; encrypts the LMK |
| LMK (local master key) | Set per peer in `esp_now_peer_info_t::lmk`; the CCMP key for that peer's frames |

Keys live in `include/espnow_keys.h`, which is gitignored. Generate it once:

```sh
python3 tools/generate_keys.py
```

The build stops with an `#error` if the file is missing. Firmware refuses to
start ESP-NOW if either key is all zeros (the values in
`include/espnow_keys.example.h`). **Both boards must be flashed with the same
keys.** If you regenerate them with `--force`, reflash both boards.

## Build environments

| Environment | Purpose |
| --- | --- |
| `esp32-s3-n16r8` | Normal encrypted build over the USB-to-UART bridge |
| `esp32-s3-n16r8-usb` | Same, with `Serial` on native USB Serial/JTAG |
| `esp32-s3-n16r8-wrong-key` | Negative test: the LMK has one flipped bit |
| `esp32-s3-n16r8-plaintext` | Negative test: the peer is added with `encrypt = false` |

Each packet includes the sender's build mode, and RX lines print it. If an
encrypted board ever receives a packet from a negative-test build, it prints
`WARNING: encrypted board accepted a negative-test packet`.

## Test procedure

Hardware, MAC whitelist, upload button sequence, and monitor commands are the
same as in [espnow_test](../espnow_test/README.md). `BOARD1_PORT` and
`BOARD2_PORT` are placeholders; run `pio device list` to find the real ports.

1. **Encrypted exchange.** Flash both boards with `esp32-s3-n16r8`. Each board
   should print `Peers: 1 total, 1 encrypted` and RX lines with
   `sender encrypted`.
2. **Wrong key.** Leave board 2 on `esp32-s3-n16r8` and flash board 1:
   ```sh
   pio run -e esp32-s3-n16r8-wrong-key -t upload --upload-port BOARD1_PORT
   ```
   Expected: neither board prints RX, because each side's CCMP check fails.
   TX may still report `queued`.
3. **Plaintext sender.** Flash board 1 with `esp32-s3-n16r8-plaintext`.
   Expected: board 2 prints no RX. Tested result: board 2 drops the plaintext
   frames even though they come from its whitelisted peer MAC. If board 2
   ever prints the WARNING line, encryption alone does not stop plaintext
   frames from a spoofed MAC.
4. Reflash board 1 with `esp32-s3-n16r8` and confirm that two-way RX resumes.

## Test results

Run on 2026-10-26 with two ESP32-S3 N16R8 boards: board 1 `E0:72:A1:D5:64:0C`
and board 2 `E0:72:A1:D7:FA:B8` (the MACs now in `include/espnow_config.h`).
Each step captured about 15 s of serial from both boards.

| Step | Board 1 build | Expected | Observed | Result |
| --- | --- | --- | --- | --- |
| 1 | `esp32-s3-n16r8` | `1 encrypted` peer; two-way RX | Both boards received sequences 1–13, `sender encrypted` | Pass |
| 2 | `esp32-s3-n16r8-wrong-key` | No RX on either board | TX 1–15 `queued` on both; no RX | Pass |
| 3 | `esp32-s3-n16r8-plaintext` | No RX or WARNING on board 2 | Board 1 `Peers: 1 total, 0 encrypted`, TX 1–15; board 2 no RX, no WARNING | Pass |
| 4 | `esp32-s3-n16r8` | Two-way RX resumes | Board 1 received 1–14, board 2 received 1–15, `sender encrypted` | Pass |

`TX n | queued` means only that `esp_now_send()` returned `ESP_OK`. It does not
confirm delivery; RX lines on the other board do.

Example output on board 1 after step 1:

```text
ESP-NOW encrypted whitelist test
Build mode: encrypted
Wi-Fi station MAC: E0:72:A1:D5:64:0C
Peers: 1 total, 1 encrypted
Ready on channel 1; sending once per second.
TX 1 | queued
RX E0:72:A1:D7:FA:B8 | sequence 1 | sender encrypted
```

## Limits

- **Plaintext frames:** with this framework (Arduino core 2.0.17 / ESP-IDF
  4.4), a board that added its peer with `encrypt = true` dropped plaintext
  ESP-NOW frames from that peer's MAC (step 3). Retest after a framework
  upgrade.
- **Shared key:** CCMP proves that a frame came from something holding the
  LMK, not which board sent it. Without flash encryption, anyone with physical
  access to a board can read the key from flash.
- **Replay:** Espressif's documentation doesn't describe replay protection for
  ESP-NOW and suggests sequence numbers at the application layer. This test
  prints sequence numbers but doesn't reject repeated or old ones.
- **Payload size:** with this framework (Arduino core 2.0.17 / ESP-IDF 4.4),
  `ESP_NOW_MAX_DATA_LEN` is 250 bytes whether or not encryption is on.
- **Peer count:** the IDF 4.4 header caps encrypted peers at
  `ESP_NOW_MAX_ENCRYPT_PEER_NUM` (6). Encrypted broadcast/multicast is not
  supported.

## Project files

- `src/main.cpp`: espnow_test plus PMK/LMK setup, key validation, peer count
  diagnostics, and the negative-test build switches.
- `include/espnow_keys.h`: local keys (gitignored, generated).
- `include/espnow_keys.example.h`: format reference with placeholder zeros.
- `include/espnow_config.h`: shared whitelist, channel, and send interval.
- `tools/generate_keys.py`: writes random keys with Python's `secrets` module.
- `platformio.ini`: board settings plus the two negative-test environments.
