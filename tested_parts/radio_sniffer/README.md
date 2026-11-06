# radio_sniffer: keyless ESP-NOW and LoRa capture

One board that only listens. It captures ESP-NOW frames with Wi-Fi
promiscuous mode and LoRa packets with a Core1121 at the same time, and
prints one line per frame on serial. It is the standalone version of the
stage 5/6 `board3-sniff` build. That build was locked to boards 1 and 2 on
one channel. This one sees every ESP-NOW frame and adds LoRa.

- **No keys.** No key header, no crypto, no session code is compiled in.
  `../shared_keys/find_keys.py` reports `no keys` for the image.
- **Never transmits.** Wi-Fi is in station mode, never connects or scans;
  the LoRa code never calls `transmit`.
- **Doesn't decrypt.** It shows what any outsider sees: MACs, sizes,
  timing, RSSI, the clear protected-message header and the ciphertext.

## Wiring

ESP-NOW needs nothing extra. LoRa uses the Core1121 on the design-review
pins, as `handset_lora_integration_test`:

| Core1121 | ESP32-S3 |
| --- | --- |
| SCK | GPIO7 |
| MISO | GPIO5 |
| MOSI | GPIO6 |
| NSS (CS) | GPIO15 |
| RESET | GPIO16 |
| BUSY | GPIO17 |
| DIO9 (IRQ) | GPIO18 |
| 3V3, GND | 3V3, GND |

LoRa is **off at boot** (`lora_on_at_boot` in `include/sniffer_config.h`),
so ESP-NOW-only capture needs no module and `STATS` shows `lora=paused`.
`lora on` starts it. With LoRa on and no module, it shows `failed` and
retries every 5 s, with a hint when BUSY is stuck high.

## Defaults

| Radio | Setting |
| --- | --- |
| ESP-NOW | Channel 11 (stage 6, relay voice, broadcast and handset tests) |
| LoRa | Off until `lora on`; then 915 MHz, 125 kHz, SF7, CR 4/5, sync 0x12, preamble 8 (`handset_lora_integration_test`, `lora_encrypt_test`, `lcd_lora_test`) |

A LoRa receiver hears only packets sent with the same frequency,
bandwidth, spreading factor and sync word. `lora_test` used 868 MHz: send
`mhz 868`. Change the defaults in `include/sniffer_config.h`.

## Build and flash

The user flashes boards. From this folder:

```sh
pio run                      # build only
pio run -e sniffer -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor --port /dev/cu.usbmodemXXXX   # 921600 baud
```

The boards need BOOT + RESET before upload (`before_reset = no_reset`).
Any board works; the sniffer prints its own MAC at boot. If it is one of
the known boards, its own handset firmware is replaced until reflashed.

## Output

```text
ESPNOW t=81234567 ch=11 rssi=-38 src=E0:72:A1:D8:E2:10/B1 dst=E0:72:A1:D7:FA:B8/B2 len=104 sm=1:voice:1>2:#5012 data=0102...
ESPNOW t=81300112 ch=11 rssi=-61 src=E0:72:A1:D7:F6:50/B3 dst=FF:FF:FF:FF:FF:FF/all len=88 sm=- data=...
LORA t=90011220 mhz=915.000 sf=7 rssi=-47.5 snr=9.75 len=72 crc=ok sm=1:text:1>2:#14 data=...
STATS espnow=on ch=11 captured=3012 filtered=0 other_mgmt=118 queue_drops=0 | lora=listening received=4 crc_bad=0 read_errors=0 queue_drops=0 starts=1 | serial_drops=0
```

| Field | Meaning |
| --- | --- |
| `t` | Board time in µs (`esp_timer`). ESP-NOW: frame received; LoRa: RX-done interrupt |
| `src`, `dst` | ESP-NOW MACs; `/B1`..`/B4` (and `/old3`) for known boards, `/all` for broadcast |
| `len` | ESP-NOW or LoRa payload bytes |
| `sm` | Header guess `version:type:source>destination:#counter`, or `-`. Version 1, type 1..4 (text, voice, pairing, session) and ≥40 bytes; other projects' payloads can match by chance |
| `crc` | LoRa only. `bad` packets are still printed (`crc_bad` counts them) |
| `data` | Payload hex; `data off` removes it |

`STATS` comes every 5 s. `queue_drops` and `serial_drops` mean the sniffer
missed or skipped lines, so a capture with drops is not complete.
`other_mgmt` counts management frames that are not ESP-NOW (beacons and
so on). Lines starting `INFO`, `OK`, `ERR` answer commands.

## Commands

Type in the serial monitor, one per line:

| Command | Effect |
| --- | --- |
| `help`, `status` | List commands; settings and counters now |
| `espnow on\|off`, `lora on\|off` | Capture each radio or not |
| `boards on\|off` | ESP-NOW only from/to known boards |
| `data on\|off` | Hex payload at line end |
| `ch N` | Wi-Fi channel 1..13 |
| `mhz F`, `bw K`, `sf N`, `cr N`, `sync X` | LoRa settings; the radio restarts |
| `lora default` | LoRa back to the defaults |

## Capture and summarise

Log the serial port to a file with any monitor, for example
`pio device monitor --port PORT | tee capture.txt`, then:

```sh
python3 tools/summarise.py capture.txt          # table
python3 tools/summarise.py capture.txt --json   # for scripts
```

It groups ESP-NOW by source → destination and LoRa by guessed IDs, and
gives frames, unique payloads (retransmits and relay copies repeat), types,
RSSI min/median/max and rate. Prefixes such as monitor timestamps are fine.

## Decrypt check (host, with the keys)

The sniffer has no keys. To prove its captures are complete and
byte-exact, `tools/decrypt_check` opens them on the computer with
`../shared_keys` and the handset firmware's own `relay_frame`,
`secure_message`, `control_frame`, `session` and `kdf` sources (from
`../handset_broadcast_integration_test`, identical to the ESP-NOW mesh
project's; `FIRMWARE_DIR=` picks another). Needs `brew install openssl@3`.

```sh
./tools/decrypt_check/build.sh
./build/decrypt_check capture.txt          # report and verdicts
./build/decrypt_check capture.txt --show   # also every opened frame
./build/decrypt_check capture.txt --audio out/   # voice streams to WAV
```

`--audio` rebuilds each push-to-talk stream in sequence order (a lost
packet from the next frame's redundant copy), decodes it with libopus as
the handsets do (16 kHz mono, 20 ms, concealment for frames still
missing) and writes one WAV per stream. When the sender's end message was
captured and nothing is missing, its CRC-32 over every Opus packet is
checked: a match means the audio is bit-exact. Needs `brew install opus`.

Per frame it unwraps the mesh envelope (`0xF1` flood, `0xF2` routed),
checks `0xF3` route-message tags, and checks the hop tag under the
network key. A frame that passes was captured byte for byte. Session keys
are new on every boot. A captured session reply carries both nonces in
clear, so the checker replays the handshake through two `session::link`
objects with the pair key. It must rebuild the captured reply byte for
byte. Then text and voice open under the session key, as on the boards.
A broadcast key travels sealed under a pair session (payload `06`, key
ID, next counter, key). Once one opens, that board's broadcasts (to ID
255) open under it.

Capture with `data on`, start the sniffer first, then reset a handset so
its handshakes are on the log. Frames from sessions made before the
sniffer started count as `no session key`, not as failures. Exit status
is 1 if any hop tag, route tag, handshake, end-to-end or broadcast tag
fails.

## Limits

- One Wi-Fi channel at a time; ESP-NOW frames on other channels are not
  seen.
- LoRa needs matching settings, and hears nothing while it restarts
  after a settings change (a few hundred ms).
- Promiscuous mode sees the frame once; it does not see whether the
  receiver acknowledged it. Retries appear as repeated payloads.
- With data on, a line is about 150 bytes plus two per payload byte
  (~360 for a 104-byte voice frame). Serial at 921,600 baud carries
  ~90 KB/s, so one 50 frame/s stream uses ~18 KB/s and two-way voice
  through a relay (four streams) is near the limit. Use `data off` or
  `boards on` if `serial_drops` rises.

## Files

| File | Purpose |
| --- | --- |
| `include/sniffer_config.h` | Pins, defaults, known board MACs |
| `src/espnow_rx.cpp` | Promiscuous-mode capture and queue |
| `src/lora_rx.cpp` | LoRa task: owns the radio, receive only |
| `src/frame_decode.cpp` | ESP-NOW parsing, header guess, output lines |
| `src/command.cpp` | Serial command parsing |
| `src/main.cpp` | Setup, commands, printing, stats |
| `tools/summarise.py` | Offline summary of a capture log |
| `tools/decrypt_check/` | Host check: opens captures with the shared keys, voice to WAV |
| `tests/run_host_tests.sh` | Host tests (C++ with ASan/UBSan, Python, decrypt self-test) |

## Validation

2026-11-06, before hardware:
- `pio run` builds `sniffer` with no warnings (RAM 13.8%, flash 11.4%).
- Host tests pass: ESP-NOW parsing and rejects, header guess, exact
  output lines and buffer limits, commands and ranges, summariser.
- `find_keys.py` on `firmware.bin`: `no keys`.

2026-11-06, hardware: sniffer on the board at `/dev/cu.usbmodem5B420192791`,
ESP-NOW only, channel 11. Boards 1-4 ran the broadcast handset build
(`handset_broadcast_integration_test`, with the ESP-NOW mesh ways); board
2 was reset during a 2-minute capture, then broadcast voice (three
push-to-talk bursts, 66-87 s).
Evidence: `evidence/decrypt_2026-11-06_capture.txt` (sniffer output) and
`evidence/decrypt_2026-11-06_report.txt` (`decrypt_check`).

| Check | Result |
| --- | --- |
| Capture | 3,978 frames; 0 queue drops, 0 serial drops |
| Hop tag, network key | 3,711 / 3,711 pass (bare 1,184, flood 1,858, routed 669) |
| Route messages `0xF3` | 267 / 267 pass |
| Handshakes | 8 distinct (direct, flood, routed ways), all 8 rebuilt byte for byte |
| Text/voice under session keys | 756 opened, 0 bad tag: presence pings on every way, broadcast-key offers and receipts |
| Previous session | 3: boards 1, 3, 4 pinged board 2 under their old key within 50 ms of the reply, before the confirm; board 2 can't open these either |
| Broadcast | 3 keys learned, 506 / 506 opened after board 2's key (454 voice, 52 end) |
| Audio (`--audio`) | 4 streams from board 2, 9.1 s of speech; 3 bit-exact (CRC matches the sender's), 1 with one frame concealed |

Not covered: frames from sessions made before the capture started (1,926,
`no session key`), the ESP-MESH way (WPA2 Wi-Fi data frames, not
ESP-NOW), and LoRa (off in this run). Two earlier captures without a reset
(20 s and 3 min, 7,452 frames) passed every hop tag but had no handshake
to decrypt with.
