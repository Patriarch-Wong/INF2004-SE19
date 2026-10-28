# Stage 6 spec: Opus voice over the end-to-end encrypted link

Brief for building stage 6 of the SE19 end-to-end security tests. It is
written to be read cold; everything needed is here or linked.

## Where things stand

Repository: `/Users/zhongjun88/Codex fun stuff/INF2004-SE19`, branch
`main`. Stage 5 is commit `19b5ae2`. Read these first:

- `CONTRIBUTING.md`: coding standard (C++ Core Guidelines, RAII, checked
  status returns, no exceptions or RTTI, snake_case, four-space indent,
  Allman braces, 80 columns).
- `tested_parts/e2e_security_test/README.md`: the overall plan (stage
  table) and the protected message format.
- `tested_parts/e2e_voice_test/README.md` and `SPEC.md`: stage 5, the
  synthetic voice stream, playout schedule, echo RTT method, keyless
  sniffer and capture analysis. **Stage 6 builds on this project.**
- `tested_parts/codec_test/README.md`: the Opus settings and the
  generated test signal, measured at 6.3 ms average encode and 2.4 ms
  average decode, 0.9991 correlation and 27.26 dB SNR.
- `tested_parts/codec_sdtest/include/codec.h`: the reusable Opus wrapper.

| Stage | Covers | Project |
| --- | --- | --- |
| 1–2 | Frame format, pairwise keys, replay window, attacks | `e2e_security_test` |
| 3 | Relay without the end-to-end key; +3.2 ms per hop | `e2e_relay_test` |
| 4 | Pairing, pair keys in NVS, session keys | `e2e_pairing_test` |
| 5 | Confidentiality capture, synthetic voice latency | `e2e_voice_test` |
| 6 | Opus voice through the encrypted link | `e2e_opus_test` (this) |

Stage 5 sent fake voice frames: random 40–80-byte payloads of filler at
50 frames per second. Encrypted results: 1.471 ms conservative crypto
bound per frame, median RTT/2 15.36 ms, 0.5% late at a 60 ms jitter buffer,
zero loss. It did not run a codec, so it says nothing about whether the
CPU can encode, encrypt, send, decrypt and decode in real time, or how the
audio sounds after late frames.

## Goal

Replace the fake frames with real Opus packets, end to end, with no
microphone or speaker. Board 1 generates a known test signal, encodes it
and sends each packet encrypted. Board 2 decrypts, runs the playout
schedule, decodes (concealing late or lost frames) and compares the audio
it would play against the same signal generated locally. Board 3 sniffs
without keys. This shows:

1. Every Opus packet arrives bit for bit, and the decoded audio matches the
   source.
2. The whole sender and receiver pipelines keep up with 50 frames per
   second, with timing for each step.
3. An estimated mouth-to-ear delay, and what late frames do to the audio.
4. None of the real Opus packets appears on the air in the encrypted build.

When the microphone and speaker are available, they replace the generator
and the comparison at the two ends; nothing in between changes.

## Hardware

The same three ESP32-S3 N16R8 boards as stage 5, on USB (CH343 UART
connector), channel 1. No microphone, speaker, LoRa or SD card.

| Board | MAC | Port | Stage 6 role |
| --- | --- | --- | --- |
| 1 | `E0:72:A1:D5:64:0C` | `/dev/cu.usbmodem5B420192981` | Opus sender |
| 2 | `E0:72:A1:D7:FA:B8` | `/dev/cu.usbmodem5B5F0215971` | Opus receiver |
| 3 | `E0:72:A1:D7:F6:50` | `/dev/cu.usbmodem5B420192791` | Passive sniffer (no keys) |

Confirm ports with `pio device list`. Boards 1 and 2 are paired (pair key
fingerprint `48FF15EF`, NVS `pairs` / `peer2`, `peer1`). At the end of
stage 5 they were left running the plaintext control builds.

## Working rules (from the user)

- **The user flashes every board by hand.** Never run uploads and never add
  tooling or firmware commands to automate flashing. When a test needs
  firmware, tell the user which environment goes on which board, with the
  exact `pio run -e ENV -t upload --upload-port PORT` commands, and wait.
  The boards cannot enter the bootloader over USB.
- Capturing serial and sending serial commands is fine, with
  `tools/capture_stage5.py`'s approach around the inherited
  `capture_logs.py`. **Opening a port resets that board.**
- Don't commit or push until the user asks. Commit messages follow the
  earlier stage commits and end with the Co-Authored-By line.
- Prefer serial-driven tests; physical buttons are optional.
- Keep status updates short; report failures plainly with the evidence.

## Project setup

Create `tested_parts/e2e_opus_test/` from `e2e_voice_test`. Copy byte for
byte and say so in the README: `secure_message`, `replay_window`,
`crypto_backend`, `kdf`, `control_frame`, `pairing`, `session`, `radio`,
`sniffer`, `sniff_frame`, their tests, `tools/capture_logs.py`,
`tools/find_keys.py`, `tools/generate_keys.py`, `partitions.csv`,
`.gitignore`. Keep the playout and statistics code from `voice.cpp`
unchanged if possible; split it from the fake-payload code rather than
editing it. Generate a new network key with `tools/generate_keys.py`.

Copy `codec.h` / `codec.cpp` from `codec_sdtest` and pin the same library
commit: `https://github.com/pschatzmann/arduino-libopus.git#bae0f8570b03071d0101445059e7178bed8bd144`.
The wrapper's `codec_decode` has no concealment; add one function for
packet loss concealment (`opus_decode` with a null packet) and say in the
README that this is the only change.

Keep the NVS pair-key format so the boards need no re-pairing.

## Opus settings

From `codec_test`, unchanged: 16 kHz mono PCM16, Opus Audio application,
24 kbit/s CBR, 20 ms frames (320 samples), complexity 5, DTX off. CBR
gives 60-byte packets.

CBR also matters for confidentiality. With VBR, packet sizes follow the
speech, and published attacks have recovered phrases from encrypted VBR
calls by their packet lengths alone. Every CBR packet has the same size,
so sizes reveal only that a call is running. Keep CBR; note it in the
README.

## Test signal

A deterministic generator, the same code on both boards and in the host
tests, producing 60 s of PCM one 20 ms frame at a time with continuous
phase: the `codec_test` pattern (two tones, a sweep, half a second of
silence) repeated across the 60 s. Board 2 regenerates the reference
locally, so no audio crosses the serial port.

## Sender (board 1)

- An `esp_timer` fires every 20 ms and queues a capture timestamp and
  sequence number, as in stage 5. It does no work itself.
- One codec task (64 KiB stack; the library is not thread safe, so it is
  the only task that calls Opus) generates the frame, encodes it, builds
  the payload, seals it as type 2 and hands it to the radio.
  *As built:* the codec task generates and encodes only; the loop builds,
  seals and sends, so session state stays in one task. The codec task runs
  on core 0 below Wi-Fi, leaving core 1 to the loop, so codec work never
  delays receiving or timestamping. Echo round trips start just before
  sealing, so RTT/2 covers seal, radio and open, not the codec.
- Time each step per frame with `esp_timer_get_time()`: generate, encode,
  seal, and capture-to-send total.
- Log per frame: sequence, Opus length, CRC32 of the Opus packet and the
  packet hex (60 bytes, so about 7 KB/s at 921600 baud). The hex is what the
  confidentiality analysis searches for on the air.
- Text `HELLO SE19 #n` once per second continues, as in stage 5.

Payload (sealed, so not visible on the air in the encrypted build):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | Marker `SE19OPUS` |
| 8 | 4 | Sequence number, little endian |
| 12 | 4 | Frames in the stream |
| 16 | 8 | Capture time, sender µs |
| 24 | 2 | Opus packet length |
| 26 | n | Opus packet |

86 bytes with a 60-byte packet, within the 210-byte limit.

## Receiver (board 2)

- Opens each frame and records arrival time at dequeue, as in stage 5. Logs
  sequence, Opus length and CRC32 of the decrypted packet.
- Answers every fifth frame with an equal-sized echo, so board 1 measures
  RTT exactly as in stage 5.
- Runs the stage 5 playout schedule (default 60 ms depth). At each 20 ms
  slot, one codec task decodes the frame if it is on time; if it is late or
  lost, it calls concealment instead. The result is the audio a speaker
  would have played. *As built:* the player looks at each slot 2 ms after
  it is due, so a frame stamped just before its slot has been stored;
  on time still means arrival stamp ≤ slot. After the stream it replays
  the stored packets at 20/40/60/80 ms for O9; the 60 ms replay must match
  the live audio exactly.
- Compares the played audio against the locally generated reference,
  aligned by the encoder lookahead (as `codec_test` does): correlation and
  SNR over the whole stream, and the worst 1 s window.
- Also decodes every received packet in sequence order, ignoring timing,
  into a second comparison. The difference between the two shows what late
  frames cost, separately from what Opus itself costs.
- Times open, decode or conceal per frame, and logs on-time / late / lost
  counts as in stage 5.

## Plaintext control and sniffer

`board1-plain` / `board2-plain`: the same pipeline with payloads in clear,
as stage 5's negative controls, and the latency baseline. `board3-sniff`:
the stage 5 sniffer, unchanged.

The analysis tool (extend stage 5's `analyse_capture.py`) adds:

- **Packet integrity:** every sender CRC32 matches a receiver CRC32 for the
  same sequence.
- **Opus on the air:** search every captured payload for every sent Opus
  packet's bytes (from the sender log hex), plus the `SE19OPUS` marker.
- Per-step timing distributions (min / median / p99 / max) and the
  mouth-to-ear estimate below.

## Delay estimate

There is no shared clock. Estimate mouth-to-ear delay as:

    20 ms frame (capture) + encode + seal + RTT/2 + jitter buffer depth
        + decode

using medians and p99 values. RTT/2 already includes seal, radio and open
for the echo, so state which terms may overlap. ITU-T G.114 recommends at
most 150 ms one-way for interactive voice; report against that.

## Tests

| # | Test | Pass (proposed targets; report actual figures either way) |
| --- | --- | --- |
| O1 | Packet integrity | All 3,000 Opus packets received; every CRC32 matches |
| O2 | Codec quality, in-order decode | Correlation > 0.99 and SNR > 20 dB against the reference (`codec_test` got 0.9991 / 27.26 dB) |
| O3 | Real-time pipeline | No step exceeds 20 ms; sender capture-to-send p99 under 15 ms; zero timer, codec and radio queue drops |
| O4 | Played audio at 60 ms buffer | Late + lost < 1%; report correlation, SNR and worst 1 s window against O2 |
| O5 | Mouth-to-ear estimate | Median and p99 reported; median under 150 ms |
| O6 | Confidentiality, encrypted | Sniffer completeness ≥ 95%; zero Opus packets and zero `SE19OPUS` markers found on the air |
| O7 | Confidentiality, plaintext control | Opus packets and markers found in essentially every captured frame |
| O8 | Keys | `find_keys.py` finds no key in the sniffer image or any log |
| O9 | Buffer sweep | O4's figures at 20, 40, 60 and 80 ms, from the same run |

Run O1–O6, O8 and O9 in one encrypted capture, and O7 with the plaintext
controls in a second capture, as in stage 5.

## Host tests

Keep the inherited suites passing. Add host tests for the test-signal
generator (same output on host and board, continuous phase across frames),
the payload format, the lookahead alignment and the SNR/correlation code,
using the system libopus (`brew install opus`) for a host encode/decode
round trip. Extend the analysis tests to the integrity and packet-search
checks. As in earlier stages, mutation-check the important rules.

## Deliverables

- `tested_parts/e2e_opus_test/`, building cleanly with no warnings.
- `README.md` in the style of the stage 5 README: what is reused, design,
  builds, host tests, hardware procedure with exact commands, results with
  figures quoted from logs, results table, limits, files.
- Logs in `evidence/stage6/`, checked with `find_keys.py` and the analysis
  tool before committing.
- Add stage 6 to the stage table in `e2e_security_test/README.md`, and to
  the project tree and project table in the root `README.md`.

## Out of scope

- Microphone, speaker, perceptual quality (no PESQ or listening tests).
- LoRa; the relay run (a later stage can reuse this firmware for it).
- Opus in-band FEC and VBR. Both are possible later changes; VBR would
  need padding to keep packet sizes from leaking speech.
- Saving the stream to SD.

## Decisions to confirm with the user early

- Bitrate (24 kbit/s CBR, from `codec_test`) and the targets above.
- Whether to log every packet's hex (needed for O6/O7 as specified) or
  only CRCs plus a sample.
- Test signal: the `codec_test` tones and sweep, or add a speech-like
  signal.
