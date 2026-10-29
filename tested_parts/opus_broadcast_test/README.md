# One-to-many Opus broadcast over ESP-NOW

Hardware run finished on 2026-10-29, on channel 11. Each of the three
boards broadcast two 60 s Opus streams, one plain and one carrying the
previous packet too, and both other boards received every stream. Of
36,000 frames due at receivers, 2 were lost on the air, both single frames
at board 2 from board 1. The redundant copy recovered the one lost in a
redundant stream. No frame was late at any buffer depth from 60 to
300 ms. Every played stream matched the source at 0.9989 correlation and
26.73 dB, except the plain stream with the one unrecovered loss
(26.63 dB). [SPEC.md](SPEC.md) defines the experiment; the changes made
since are listed under *Changes from the spec*.

Broadcast is a separate mode from the encrypted unicast of stages 1–6.
It has **no encryption and no authentication**: any device on the channel
can send audio into a stream (see *Limits*). This project tests broadcast
on its own; combining the two modes in one firmware comes later.

## Design and reuse

Built from Stage 6 (`e2e_opus_test`). These 13 files are byte-identical
to Stage 6: `codec.h` / `codec.cpp` (with `codec_conceal`, `arduino-libopus`
`bae0f85`), `opus_voice.h` / `opus_voice.cpp` (test signal, CRC32,
playout rule, audio comparison), `opus_player.h` / `opus_player.cpp`,
`heap_owner.h`, `tools/capture_logs.py`, `partitions.csv`, `.gitignore`,
and the Opus host tests `opus_test.cpp`, `test_check.h` and
`tests/host/esp_heap_caps.h`. `opus_voice` still contains Stage 6's
`SE19OPUS` payload functions; the firmware doesn't call them, and they
were left in so the file stays identical.

Adapted from Stage 6:

- `src/radio.cpp`: station mode, channel 11, Wi-Fi power save off, the
  MAC allowlist, the receive queue and the send callback are kept. The
  `secure_message` dependency and the per-board peers are gone; the only
  peer is `FF:FF:FF:FF:FF:FF`, with link encryption off. Send callbacks
  also go into a queue, in send order, so the loop can log each one
  against its sequence number.
- `include/network_config.h`: one device table, which gives each board its
  number from its MAC. Frames from other MACs are dropped.
- `include/app_config.h`: the Stage 6 Opus settings with a 200 ms live
  buffer, the sweep depths and the 5 s stream timeout.

Nothing comes from `secure_message`, `crypto_backend`, `kdf`, `pairing`,
`session`, `sniffer` or NVS. There is no key file.

### One firmware for every board

A single environment, `broadcast`, goes on all three boards. Each board
prints its number at boot:

    ESP-NOW Opus broadcast | board 2 | channel 11 | power save off
    Build mode: broadcast, no encryption

| Command | Action |
| --- | --- |
| `voice plain` | Send one 60 s stream, current packet only |
| `voice red` | Send one 60 s stream, current and previous packet |
| `report` | Print the last stream's results again |
| `status` | Board number, role, streams, counters |
| `reboot` | Restart the board |

Every board listens all the time. A board refuses `voice` while it is
receiving or finishing a stream, and a sending board doesn't play other
streams; it counts them as `heard`.

### Opus settings and test signal

Unchanged from Stage 6: 16 kHz mono PCM16, Audio application, 24 kbit/s
CBR, 20 ms frames (320 samples), complexity 5, DTX off, 60-byte packets.
The generated 3 s pattern repeats for 60 s (3,000 frames), and each
receiver regenerates the reference locally. CBR no longer hides anything,
since broadcast is in the clear; it is kept so results compare directly
with Stage 6. Each stream gets a fresh encoder, so every stream carries
the same packets.

### Frame format

Plain ESP-NOW payload sent to the broadcast address
(`broadcast_frame.h`):

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

Frames are 93 bytes plain and 155 bytes redundant, within ESP-NOW's 250.
Frame 0 has no previous packet. Receivers reject a wrong marker or
version, a sender, stream ID or sequence out of range, an unknown flag,
and any lengths that don't add up to the frame size. They also reject a
frame whose sender number isn't its MAC's board.

### Sender

As in Stage 6, without sealing or echoes. An `esp_timer` queues a capture
tick every 20 ms. One codec task, the only one that calls Opus, generates
and encodes the frame. The loop builds the broadcast frame, adds the
previous packet in redundant mode, sends it and logs `OPUS_TX`: sequence,
length, CRC32, generate, encode and capture-to-send times, the
`esp_now_send` result, frame size and whether a previous packet went with
it. Each send callback is logged as `SEND_CB <sequence> <ok>`. For
broadcast, a successful callback means the frame left the radio; nothing
acknowledges it, and nothing resends it. `TX_REPORT` sums up the stream.

### Receivers

The loop stamps each frame when it dequeues it. A receiver locks to the
first stream ID it hears and keeps it until the last slot has played, or
until nothing from that stream has arrived for 5 s. Frames from other
streams count as foreign, and late frames of a finished stream as stale.
Each copy is logged as `COPY <sequence> <P|R> <length> <CRC32> <arrival>
<result>`, where P is the frame's own copy and R is the copy carried by
the next frame.

Two slot sets share the stream's PSRAM storage
(`broadcast_receiver.h`). `earliest[s]` holds the first copy of `s` to
arrive, from either kind of copy, and is what plays. `primary[s]` holds only
the frame's own copy. Every slot is written once and published by a ready
flag, so the codec task can read while the loop keeps storing.

The codec task plays each slot 2 ms after it is due, at 200 ms. It plays
the primary copy if that arrived in time, otherwise the redundant copy if
that arrived in time, otherwise it conceals, and logs `OPUS_PLAY` with P,
R or C. The earliest copy always arrives no later than the primary, so
playing `earliest[s]` through the unchanged `opus_player` gives exactly
this choice. The spec had planned a new player function for the redundant
copy; it wasn't needed.

After the stream, the receiver counts the same choice at 60, 100, 150,
200 and 300 ms from the stored arrivals (`SOURCES sweep`). It then decodes
every received packet once, in order, with a fresh decoder (`QUALITY
copies 4294967295`), for the codec's own quality. `LIVE_MISSED` counts
copies that were on time but not yet stored when the player looked; it
should be 0.

## Changes from the spec

- **Fewer replays.** The spec replayed every stream with a decoder at
  each depth. On the ESP32-S3 one replay takes about 15 s, so a redundant
  stream needed about 4 minutes and the capture about 25. To keep the run
  short, receivers now decode once live and once in order. The B7 sweep
  reports late and missing slots per depth from the logged arrivals, with
  no SNR per depth.
- **Live check.** The spec checked live playback against a 200 ms replay.
  The analysis now recomputes every slot's choice (P, R or C) from the
  logged arrivals and checks it against what the player logged.
- **Schedule.** Six streams 100 s apart (about 10.5 minutes). B10 (two
  senders at once) was not run.

## Builds and host checks

Run from this directory:

```sh
pio run
sh tests/run_tests.sh
python3 tests/mutation_check.py
```

`run_tests.sh` runs:
- Stage 6's Opus suite, against the system libopus (`brew install opus`;
  1.6.1 here).
- The frame and receiver suite, built with AddressSanitizer and UBSan:
  build and parse in both modes and every rejection; stream lock, foreign,
  stale and wrong-length frames, duplicates, mismatched copies, reset, and
  the slot choice.
- The player suite, with libopus: live playback with the firmware's slot
  choice against `opus_player::replay`, with single losses, a two-frame
  burst, a late primary and a lost first frame, in both modes.
- 12 analysis tests.

`mutation_check.py` makes 24 mutations on temporary copies and each must
fail a test. They cover the slot choice, the redundant copy's sequence,
the stream lock, first-copy and duplicate rules, length, flag and header
checks, and the analysis's CRC, loss, burst, recovery, shared-loss,
live-check and step rules. One mutation (packet length past the frame
end) is caught only by the sanitizer, through a frame in an exact-size
buffer: without it, parsing reads past the frame but still returns the
right result.

Validation on 2026-10-29, on the final code: the build has 0 warnings,
every suite passes and all 24 mutations are killed. Output is in
[evidence/broadcast/validation.txt](evidence/broadcast/validation.txt).

## Manual hardware procedure

The user flashes every board. **Do not automate uploads.** Enter the
bootloader with BOOT + RESET as needed, then, from this directory:

```sh
pio run -e broadcast -t upload --upload-port /dev/cu.usbmodem5B420192981   # board 1
pio run -e broadcast -t upload --upload-port /dev/cu.usbmodem5B5F0215971   # board 2
pio run -e broadcast -t upload --upload-port /dev/cu.usbmodem5B420192791   # board 3
```

Confirm ports with `pio device list` and close other monitors. Opening a
port resets that board. Each stream takes about 75 s at the receivers
(60 s live, then the in-order decode), so streams go 100 s apart:

```sh
caffeinate -dims ~/.platformio/penv/bin/python tools/capture_broadcast.py run1 \
  board3=/dev/cu.usbmodem5B420192791 board2=/dev/cu.usbmodem5B5F0215971@3 \
  board1=/dev/cu.usbmodem5B420192981@6 --baud 921600 --duration 630 \
  --send "board1@15=voice plain" --send "board1@115=voice red" \
  --send "board2@215=voice red"  --send "board2@315=voice plain" \
  --send "board3@415=voice plain" --send "board3@515=voice red"
python3 tools/analyse_broadcast.py evidence/broadcast/run1 \
  > evidence/broadcast/run1_analysis.json
```

Each stream should show one `STREAM_TX_START`, two `STREAM_LOCK ...
anchor 0`, a `TX_REPORT` and two `RX_REPORT`s. A `VOICE refused` line
means a board was still busy with the stream before.

## Analysis

`tools/analyse_broadcast.py` reads all three logs of one capture and
splits them by stream ID:
- **Integrity:** every copy's CRC32 against the sender's for that
  sequence.
- **Delivery:** primary copies received and lost per receiver, and loss
  burst lengths.
- **Recovery:** losses filled by a redundant copy, slots with no copy,
  and losses whose next frame arrived but didn't recover them.
- **Shared loss:** sequences lost at both receivers or at one only, and
  how many had a send error or a failed callback.
- **Timing:** sender and receiver step distributions.
- **Live check and sweep:** slot choices from the logged arrivals at each
  depth, compared with the player's log and the firmware's own counts.
- **Delay estimate:** 20 ms frame + capture-to-send + send → air + air →
  loop + 200 ms buffer + play lag + decode. Receivers send no echoes, so
  there is no round trip. Send → air (12.7 ms) and air → loop (0.8 ms) are
  the Stage 6 channel 11 medians from unicast. Broadcast frames aren't
  acknowledged, so their send → air time is probably shorter, but it
  wasn't measured here.

## Results

| Test | Target | Result |
| --- | --- | --- |
| B1 | Each board sends plain and redundant; both others receive; no step over 20 ms; zero drops | **Pass:** 6 of 6 streams, 3,000/3,000 sent, 0 send errors, 3,000 callbacks OK each; worst step 7.8 ms (encode); 0 timer, radio, encode, codec and play-log drops |
| B2 | Every received copy's CRC32 matches the sender's | **Pass:** 0 mismatches in the 35,998 primary and 17,993 redundant copies received |
| B3 | Raw broadcast loss (measurement) | 2 of 36,000 primary copies lost (0.006%), both single frames at board 2 from board 1, one in each mode |
| B4 | Losses whose next frame arrived are recovered; no copy < 0.5% | **Pass:** 1 loss in redundant mode, recovered; 0 slots with no copy |
| B5 | Redundant: late + missing < 1% at 200 ms | **Pass:** 0 late or missing in 6 receptions; live check matches every slot; `LIVE_MISSED 0`. Plain: 1 concealed slot (0.03%) in one reception, 0 in the other five |
| B6 | Redundant, in order: correlation > 0.99, SNR > 20 dB | **Pass:** 0.9989 / 26.73 dB, worst 1 s window 25.04 dB, in every reception |
| B7 | B5's figures at 60–300 ms | **Complete:** 0 late at every depth; the only empty slot is the plain stream's lost frame |
| B8 | Shared versus one-receiver loss | 0 lost at both receivers; both losses at one receiver, with no send error or failed callback |
| B9 | Senders hear nothing during their stream | **Pass:** `heard 0` in all 6 streams |
| B10 | Two senders at once (optional) | Not run |

Evidence: `run1_*` and `run1_analysis.json` under
[evidence/broadcast/](evidence/broadcast/).

| Stream | Sender | Mode | Receiver | Lost | Recovered | Live at 200 ms |
| --- | --- | --- | --- | --- | --- | --- |
| `0B881BFC` | 1 | plain | 2 | 1 | – | 1 concealed, 26.63 dB, worst window 22.77 dB |
| | | | 3 | 0 | – | 26.73 dB |
| `F7F5CA45` | 1 | red | 2 | 1 | 1 | 26.73 dB |
| | | | 3 | 0 | – | 26.73 dB |
| `707B21CA` | 2 | red | 1, 3 | 0 | – | 26.73 dB |
| `37D0598C` | 2 | plain | 1, 3 | 0 | – | 26.73 dB |
| `8BEC4901` | 3 | plain | 1, 2 | 0 | – | 26.73 dB |
| `CB473690` | 3 | red | 1, 2 | 0 | – | 26.73 dB |

All correlations are 0.9989.

| Step (µs), all streams | Min | Median | p99 | Max |
| --- | --- | --- | --- | --- |
| Generate (18,000) | 56 | 483 | 511 | 757 |
| Encode (18,000) | 2,799 | 6,988 | 7,361 | 7,810 |
| Capture to send (18,000) | 3,561 | 7,653 | 8,669 | 8,689 |
| Decode (35,999) | 1,858 | 2,548 | 3,059 | 3,553 |
| Conceal (1) | 5,066 | 5,066 | 5,066 | 5,066 |
| Play lag after slot (36,000) | 2,484 | 2,499 | 2,506 | 2,760 |

Delay estimate, medians: 20 + 7.7 + 12.7 + 0.8 + 200 + 2.5 + 2.5 ≈
**246 ms**, almost all of it the buffer. Nothing was late even at 60 ms,
which would give about 106 ms.

Compared with Stage 6, where 2.87% of encrypted unicast frames were late
at 60 ms on the same channel, nothing was late here. A likely reason is
that broadcast frames aren't acknowledged or resent, so the radio never
holds a frame waiting for an ACK. This is a single run on a different
day, so it is a likely explanation, not a measured one.

### First attempt: watchdog reset

The first capture (`crash_*`) ran on an earlier build that replayed each
stream at six depths. The receivers were still replaying stream 1 when
stream 2 started, so they missed its start. When they finished, they
locked onto stream 2 at frame 2486. The live player then played the
2,486 slots due before that frame at once. It concealed each without
waiting, and so never yielded. After 5 s the task watchdog reset both
receivers (`task_wdt: CPU 0: codec`). They locked again at frame 2757 and
were reset again. The player now yields while it catches up on overdue
slots, as the replays already did. Stream 1 of that capture was clean and
matches run 1: board 3 3,000/3,000, board 2 2,999/3,000.

## Limits

- **No authentication.** Any device on channel 11 can send `SE19BCST`
  frames using one of the allowlisted MACs, and receivers can't tell.
  The allowlist only drops stray traffic: MACs are sent in the clear and
  are easy to spoof.
  A network-key tag would fix this for devices outside the network.
- **Two receivers.** Three boards give one sender and two receivers. The
  firmware doesn't limit receivers, but more than two is untested.
- **One run in an uncontrolled 2.4 GHz environment**, with very few
  losses. Redundancy recovered the one loss it could; a busier channel
  is needed to measure it properly.
- **Late joiners hear the stream from where they locked.** Slots before
  the first frame heard are concealed; the analysis treats them as lost.
- **Audio is a generated signal.** There is no microphone, speaker or
  listening test.
- **Out of scope:** encryption and the unicast/broadcast mode switch,
  Opus in-band FEC, LoRa, relays and SD storage.

The agent did no uploads; the user flashed all three boards.

## Files

- `src/main.cpp`: sender, receiver, codec task, commands.
- `include/broadcast_frame.h`, `src/broadcast_frame.cpp`: frame build and
  parse.
- `include/broadcast_receiver.h`, `src/broadcast_receiver.cpp`: stream
  lock, copy storage and slot choice.
- `include/radio.h`, `src/radio.cpp`: broadcast-only ESP-NOW with the MAC
  allowlist and ordered send results.
- `include/opus_voice.h`, `include/opus_player.h`, `include/codec.h` and
  their sources: unchanged from Stage 6.
- `tests/`: Opus, broadcast and player host tests, analysis tests,
  mutations and `run_tests.sh`.
- `tools/analyse_broadcast.py`: per-stream analysis and B1–B9 verdicts.
- `tools/capture_broadcast.py`: selects `evidence/broadcast/` for the
  inherited capture tool.
- `evidence/broadcast/`: run 1 logs and analysis, the first attempt's
  `crash_*` logs, the `boot_*` probe and the validation output.
