# Spec: one-to-many Opus broadcast over ESP-NOW

Brief for building a standalone broadcast voice test. It is written to be
read cold; everything needed is here or linked.

## Where things stand

Repository: `/Users/zhongjun88/Codex fun stuff/INF2004-SE19`, branch
`main`. Stage 6 of the end-to-end security tests is commit `a447441`.
Read these first:

- `CONTRIBUTING.md`: coding standard (C++ Core Guidelines, RAII, checked
  status returns, no exceptions or RTTI, snake_case, four-space indent,
  Allman braces, 80 columns).
- `tested_parts/e2e_opus_test/README.md` and `SPEC.md`: stage 6, Opus over
  the encrypted unicast link. **This project builds on its codec, test
  signal, player and analysis code.**

Stage 6 sent 3,000 Opus packets one to one, encrypted. All arrived intact,
but 2.87% were late at a 60 ms jitter buffer. Board 1's radio held frames
before transmitting, in plaintext runs too. Worked out from the same logs,
a 200 ms buffer would have left zero late frames in every encrypted run.

## Requirements (from the user, 2026-10-29)

- **Delay is acceptable.** What matters is that the voice gets across and
  plays out of the speaker.
- **Broadcast is a separate mode, selectable beside encrypted unicast.**
  Broadcast voice needs no encryption. This project tests broadcast on its
  own; combining the two modes in one firmware comes later.
- **One to many:** one board sends, every other board receives.
- **Any board can be the sender**, and then the other two receive. The test
  rotates the sender across all three boards.
- **No keys at all.** No crypto, no network-key tag, no key files.
  Anyone on the channel can inject audio; that is accepted for now and is
  stated as a limit in the README.
- **Measure both loss handling options:** plain broadcast, and each frame
  also carrying the previous Opus packet.
- **200 ms jitter buffer** for live playback.

## Goal

Show that a voice stream broadcast by any one board reaches every other
board well enough to play, and measure what broadcast costs compared with
unicast. ESP-NOW broadcast frames get no acknowledgement and no resend, so
a frame lost on the air is gone. Stage 6's unicast frames were resent by
the radio (44 resends in the encrypted channel 11 run). The test shows:

1. Every board can send, and both other boards receive the same stream.
2. How many frames each receiver loses on the air, and whether losses are
   shared between receivers or happen at one only.
3. How many of those losses the redundant previous packet recovers.
4. The audio each receiver would play at a 200 ms buffer, against the
   reference, and how that changes with buffer depth.

There is still no microphone or speaker; they replace the generator and the
comparison in a later stage.

## Hardware

Three ESP32-S3 N16R8 boards on USB (CH343 UART connector), channel 11,
Wi-Fi power save off (both from stage 6). No microphone, speaker, LoRa or
SD card.

| Board | MAC | Port |
| --- | --- | --- |
| 1 | `E0:72:A1:D5:64:0C` | `/dev/cu.usbmodem5B420192981` |
| 2 | `E0:72:A1:D7:FA:B8` | `/dev/cu.usbmodem5B5F0215971` |
| 3 | `E0:72:A1:D7:F6:50` | `/dev/cu.usbmodem5B420192791` |

Confirm ports with `pio device list`. At the end of stage 6, boards 1 and 2
ran the encrypted stage 6 builds and board 3 the stage 5 sniffer.

## Working rules (from the user)

- **The user flashes every board by hand.** Never run uploads and never add
  tooling or firmware commands to automate flashing. Give the exact
  `pio run -e ENV -t upload --upload-port PORT` commands and wait. The
  boards cannot enter the bootloader over USB.
- Capturing serial and sending serial commands is fine, with stage 6's
  `capture_stage6.py` approach around the inherited `capture_logs.py`.
  **Opening a port resets that board.**
- Don't commit or push until the user asks. Commit messages follow the
  earlier stage commits and end with the Co-Authored-By line.
- Prefer serial-driven tests; physical buttons are optional.
- Keep status updates short; report failures plainly with the evidence.

## Project setup

Create `tested_parts/opus_broadcast_test/`. It is not an end-to-end
security stage, so it gets no stage number.

Copy byte for byte from `e2e_opus_test`, and say so in the README:
`codec.h` / `codec.cpp` (with `codec_conceal`, same `arduino-libopus`
commit `bae0f85`), the test signal, CRC32 and audio comparison from
`opus_voice`, `tools/capture_logs.py`, `partitions.csv`, `.gitignore`.
Split reused code from the stage 6 payload code rather than editing it, as
stage 6 did with `voice.cpp`.

Adapt and list the changes:

- `radio`: keep station mode, channel 11, power save off, the MAC
  allowlist from `network_config.h` (it gives each board its number and
  drops stray ESP-NOW traffic), the receive queue and the send-result
  callback. Remove the `secure_message` dependency. Peers: the broadcast
  address only.
- `opus_player`: add playing a slot from the redundant copy. Keep decode,
  conceal and replay otherwise unchanged.

Copy nothing from `secure_message`, `crypto_backend`, `kdf`, `pairing`,
`session`, `sniffer` or NVS. The project has no key file.

## One firmware for every board

A single environment, `broadcast`, goes on all three boards. Each board
finds its number from its MAC and prints it at boot:

    ESP-NOW Opus broadcast | board 2 | channel 11 | power save off
    Build mode: broadcast, no encryption

Every board listens all the time. Serial commands:

| Command | Action |
| --- | --- |
| `voice plain` | Send one 60 s stream, current packet only |
| `voice red` | Send one 60 s stream, current and previous packet |
| `report` | Print the last stream's results again |
| `status` | Board number, role, streams seen, counters |

A board that is receiving a stream refuses `voice` until that stream ends,
and a sending board doesn't play other streams. Two boards sending at once
is out of scope, but see B10.

## Opus settings and test signal

Unchanged from stage 6: 16 kHz mono PCM16, Audio application, 24 kbit/s
CBR, 20 ms frames (320 samples), complexity 5, DTX off, 60-byte packets.
The same generated 3 s pattern repeated for 60 s (3,000 frames). Each
receiver regenerates the reference locally.

CBR no longer protects anything, since broadcast is in the clear. Keep it
anyway, so results compare directly with stage 6.

## Frame format

Plain ESP-NOW payload sent to `FF:FF:FF:FF:FF:FF`; no `secure_message`
header.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | Marker `SE19BCST` |
| 8 | 1 | Format version, 1 |
| 9 | 1 | Sender board number |
| 10 | 4 | Stream ID, random per stream, little endian |
| 14 | 4 | Sequence number, from 0, little endian |
| 18 | 4 | Frames in the stream (3,000) |
| 22 | 8 | Capture time, sender µs |
| 30 | 1 | Flags: bit 0 set if a previous packet follows |
| 31 | 2 | Opus packet length n |
| 33 | n | Opus packet for this sequence |
| 33+n | 2 | Previous packet length m (only if flagged) |
| 35+n | m | Opus packet for sequence − 1 (only if flagged) |

93 bytes plain and 155 bytes redundant with 60-byte packets, within
ESP-NOW's 250 bytes. Frame 0 of a redundant stream has no previous packet
and clears the flag. Receivers reject frames with the wrong marker or
version, an inconsistent length, or a sequence outside the stream, and
count them.

## Sender

As stage 6's sender, without sealing or echoes:

- An `esp_timer` fires every 20 ms and queues a capture timestamp and
  sequence. One codec task generates and encodes; the loop builds the
  frame, sends it and logs it.
- Per frame, log the sequence, packet length, CRC32 of the current packet,
  generate, encode and capture-to-send times, the `esp_now_send` result and
  the send callback status. Hex is not needed: there is no confidentiality
  check.
- `HELLO` text frames are not sent; only the stream is on the air.
- Count frames heard from other boards during the stream. Receivers never
  transmit, so this should stay at zero.

## Receivers

- Every frame from the allowlist is stamped at dequeue, as in stage 6.
- A receiver locks to the first stream ID it sees, and keeps it until frame
  2,999's slot has played, or nothing from that stream has arrived for 5 s.
  Frames from any other stream are counted as foreign and never played.
  After the stream, it reports, runs the replays and is ready for the next.
- Log each copy: sequence, primary or redundant, CRC32, arrival.
- The player decides each slot 2 ms after it is due, as in stage 6. It
  plays, in order of preference: the primary copy if it arrived by the
  slot; else the redundant copy if it arrived by the slot; else
  concealment. Count each outcome.
- Store every copy in PSRAM. After the stream, replay at 60, 100, 150, 200
  and 300 ms, and in sequence order using any copy, with a fresh decoder
  each time. The 200 ms replay must equal the live audio (`LIVE_MISSED 0`).
- Compare each result with the reference after the 104-sample lookahead:
  correlation, SNR and the worst 1 s window.
- Receivers never call `esp_now_send`.

## Analysis

`tools/analyse_broadcast.py` reads one capture with all three boards' logs
and splits it by stream:

- **Integrity:** every received copy's CRC32 matches the sender's CRC32 for
  that sequence.
- **Delivery per receiver:** primary copies received, missing, and the
  lengths of loss bursts.
- **Recovery:** missing primaries filled by a redundant copy, and frames
  with no copy at all.
- **Shared loss:** sequences missing at both receivers, at one only, and
  their relation to the sender's send status. Shared losses point to the
  sender or the channel; one-receiver losses point to that receiver.
- **Timing:** the sender's per-step distributions, as stage 6.
- **Delay estimate:** 20 ms frame + capture-to-send + air + buffer + play
  lag + decode. Receivers send no echoes, so there is no RTT; use stage 6's
  channel 11 send → air and air → loop medians and say so. Delay is
  reported, not a target.

Add analysis tests in the style of stage 6's `analysis_test.py`.

## Tests

| # | Test | Pass (proposed targets; report actual figures either way) |
| --- | --- | --- |
| B1 | Any board sends | Each board completes a plain and a redundant stream, and both other boards receive each one; no step over 20 ms; zero timer, encode and send-queue drops |
| B2 | Integrity | Every received copy's CRC32 matches the sender's |
| B3 | Raw broadcast loss | Plain mode: primary copies lost per receiver per sender, and burst lengths. Measurement only, no target |
| B4 | Redundancy recovery | Redundant mode: every loss whose next frame arrived is recovered; frames with no copy < 0.5% per receiver |
| B5 | Played audio at 200 ms | Redundant mode, each receiver of each stream: late + missing < 1%; report correlation, SNR and worst 1 s window. Plain mode reported against the same target |
| B6 | Codec quality, in order | Redundant mode: correlation > 0.99, SNR > 20 dB (stage 6: 0.9989 / 26.73 dB) |
| B7 | Buffer sweep | B5's figures at 60, 100, 150, 200 and 300 ms, from the same streams |
| B8 | Loss pattern | Shared versus one-receiver losses, per sender |
| B9 | Silent receivers | Each sender hears zero frames during its stream |
| B10 | Two senders (optional) | Boards 1 and 2 send at once; board 3 plays only the stream it locked to and counts the other as foreign |

One capture covers B1–B9: six streams, each sender plain then redundant,
with the order swapped for the middle sender (1 plain, 1 red, 2 red,
2 plain, 3 plain, 3 red), so a slow change in the channel doesn't favour
one mode. Each stream needs about 3 minutes including replays, so about
20 minutes in all. Run B10 as a short separate capture.

## Host tests

Frame build and parse in both modes, with each rejection case; the
player's choice between primary, redundant and concealment at each slot;
the stream lock, foreign frames and timeout; the replay equalling live
playback. Keep stage 6's Opus suite for the reused code, against the system
libopus (`brew install opus`). As in earlier stages, mutation-check the
important rules: slot choice, redundant copy sequence, stream lock, length
checks, CRC and the loss counts.

## Deliverables

- `tested_parts/opus_broadcast_test/`, building cleanly with no warnings.
- `README.md` in the style of stage 6's: what is reused, design, builds,
  host tests, hardware procedure with exact commands, results with figures
  quoted from logs, results table, limits, files.
- Logs and analysis in `evidence/broadcast/`.
- Add the project to the tree and the project table in the root
  `README.md`.

## Limits to state

- No authentication: any device on the channel can inject audio, and
  receivers can't tell. Adding the network-key tag later fixes this for
  devices outside the network.
- Three boards give one sender and two receivers. The firmware doesn't
  limit the number of receivers, but more than two is untested.
- Single captures in an uncontrolled 2.4 GHz environment, as in stage 6.

## Out of scope

- Encryption and the unicast/broadcast mode switch.
- Microphone, speaker and listening tests.
- Opus in-band FEC (an alternative to the redundant packet), LoRa, relays
  and SD storage.

## Decisions to confirm with the user early

- Project name `opus_broadcast_test`.
- 60 s streams and the six-stream capture order.
- Whether to run B10.
