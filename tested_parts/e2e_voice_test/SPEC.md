# Stage 5 spec: confidentiality capture and synthetic voice latency

Brief for building stage 5 of the SE19 end-to-end security tests. It is
written to be read cold; everything needed is here or linked.

## Where things stand

Repository: `/Users/zhongjun88/Codex fun stuff/INF2004-SE19`, branch
`main`. Stage 4 is commit `4d31860`. Read these first:

- `CONTRIBUTING.md`: coding standard (C++ Core Guidelines, RAII, checked
  status returns, no exceptions or RTTI, snake_case, four-space indent,
  Allman braces, 80 columns).
- `tested_parts/e2e_security_test/README.md`: the overall plan (stage
  table), the protected message format, and stages 1–2.
- `tested_parts/e2e_relay_test/README.md`: stage 3, the relay and the
  ping/pong latency method.
- `tested_parts/e2e_pairing_test/README.md`: stage 4, pairing, pair keys in
  NVS and per-boot session keys. **Stage 5 builds on this project.**

| Stage | Covers | Project |
| --- | --- | --- |
| 1–2 | Frame format, pairwise keys, replay window, attacks | `e2e_security_test` |
| 3 | Relay without the end-to-end key; +3.2 ms per hop | `e2e_relay_test` |
| 4 | Pairing, pair keys in NVS, session keys | `e2e_pairing_test` |
| 5 | Confidentiality capture, synthetic voice latency | `e2e_voice_test` (this) |

Frame format (unchanged since stage 1): 8-byte header (version, type,
source, destination, counter) authenticated in clear, AES-128-GCM
ciphertext, 16-byte end-to-end tag, 16-byte hop tag. 40 bytes overhead,
max payload 210 bytes. Type 1 is text, type 2 voice; voice has never been
sent yet.

## Goals

1. **Confidentiality capture.** Show with a passive, keyless third board
   that what boards 1 and 2 actually put on the air reveals no message
   content. Earlier stages only checked each sender's own TX hex, and
   stage 2 used a broadcast fixture. Passive capture of unicast traffic
   was listed as out of scope there.
2. **Synthetic voice latency.** Stream voice-sized frames at the real voice
   rate through the stage 4 session encryption and measure crypto cost,
   delay, jitter, loss and lateness against a no-encryption baseline. There
   is no microphone or speaker: the board simulates both.

## Hardware

Three ESP32-S3 N16R8 boards on USB (CH343 UART connector), channel 1.

| Board | MAC | Port | Stage 5 role |
| --- | --- | --- | --- |
| 1 | `E0:72:A1:D5:64:0C` | `/dev/cu.usbmodem5B420192981` | Voice sender |
| 2 | `E0:72:A1:D7:FA:B8` | `/dev/cu.usbmodem5B5F0215971` | Voice receiver |
| 3 | `E0:72:A1:D7:F6:50` | `/dev/cu.usbmodem5B420192791` | Passive sniffer (no keys) |

Confirm ports with `pio device list`. At the end of stage 4, boards 1 and 2
were paired (pair key fingerprint `48FF15EF`, NVS namespace `pairs`, blob
`peer2`/`peer1`); board 3 had no pair keys.

## Working rules (from the user)

- **The user flashes every board by hand.** Never run uploads and never add
  tooling or firmware commands to automate flashing. When a test needs
  firmware, tell the user which environment goes on which board, with the
  exact `pio run -e ENV -t upload --upload-port PORT` commands, and wait.
  The boards cannot enter the bootloader over USB.
- Capturing serial and sending serial commands is fine. Use
  `tools/capture_logs.py` from `e2e_pairing_test`: it opens several ports
  with shared timestamps, staggers opening (`LABEL=PORT@SECONDS`), and sends
  timed commands (`--send LABEL@SECONDS=TEXT`). **Opening a port resets that
  board.**
- Don't commit or push until the user asks. Commit messages follow the
  earlier stage commits and end with the Co-Authored-By line.
- Physical buttons are optional; prefer serial-driven tests.
- Keep status updates short; report failures plainly with the evidence.

## Project setup

Create `tested_parts/e2e_voice_test/` from `e2e_pairing_test`. Copy byte
for byte and say so in the README: `secure_message`, `replay_window`,
`crypto_backend`, `kdf`, `control_frame`, `pairing`, `session`, `radio`,
their tests, `tools/capture_logs.py`, `tools/find_keys.py`,
`tools/generate_keys.py`, `partitions.csv`, `.gitignore`. Generate a new
network key with `tools/generate_keys.py` (gitignored `include/e2e_keys.h`).

Keep the NVS pair-key format (`pairs` / `peer<N>`, 16 bytes) so the stage 4
pairing of boards 1 and 2 carries over without re-pairing. If the boards
must be re-paired, the `pair`/`yes` serial commands from stage 4 do it.

## Part 1: confidentiality capture

### Sniffer firmware (`board3-sniff`)

- Holds **no keys at all**: compile no `e2e_keys.h`, no crypto, no session
  code, like stage 2's `attacker.cpp`.
- Station mode, channel 1, `esp_wifi_set_promiscuous` with a management
  frame filter. ESP-NOW frames are 802.11 action frames (category 127,
  vendor specific) carrying a vendor element with Espressif OUI `18:FE:34`,
  type 4. Parse the element and extract the ESP-NOW payload, plus source and
  destination MACs from the 802.11 header.
- Keep only frames between boards 1 and 2. Print each as one line: time,
  source MAC, destination MAC, payload length, payload hex.
- **Serial bandwidth:** 50 voice frames per second as hex is about 10 KB/s,
  close to 115200 baud's limit. Raise the sniffer's baud rate (e.g. 921600,
  and pass it to `capture_logs.py --baud`), or have the sniffer keep counts
  and print only a sample plus summaries. Count and report frames dropped
  for any reason (queue full, serial backlog).

### Plaintext control (`board1-plain`, `board2-plain`)

Same traffic with encryption switched off: payload in clear in the same
frame layout, still labelled as a negative build at boot. This proves the
sniffer and analysis would have caught readable content. It also serves as
the latency baseline in part 2.

### Known plaintext

Every text and voice payload carries recognisable content the analysis can
search for: text `HELLO SE19 #n`, and voice frames start with an ASCII
marker (e.g. `SE19VOICE`) followed by the sequence number. Choose markers
that cannot occur by chance in a few minutes of ciphertext (≥ 8 bytes).

### Analysis tool (`tools/analyse_capture.py`)

Reads sniffer logs and sender logs and reports:

- Frames captured versus frames the sender logged as sent (capture
  completeness).
- Occurrences of every known-plaintext marker in captured payloads, as raw
  bytes.
- Header fields readable in clear (type, source, destination, counter):
  expected and documented, since they are authenticated but not encrypted.
- Ciphertext of identical plaintext differs: e.g. the voice body repeats a
  fixed pattern, so equal plaintext at different counters must give
  different ciphertext bytes.
- No captured payload matches any frame twice except genuine
  retransmissions (same counter).

### Tests

| # | Test | Pass |
| --- | --- | --- |
| C1 | Encrypted text and voice, sniffed | Capture completeness ≥ 95%; zero marker hits; repeated plaintext never gives repeated ciphertext |
| C2 | Plaintext control | Markers found in essentially every captured frame |
| C3 | Keys | `find_keys.py` finds no key in the sniffer image or any log |

## Part 2: synthetic voice latency

### Simulated microphone (sender, board 1)

- An `esp_timer` fires every 20 ms, like an Opus encoder handing over one
  frame (50 frames per second).
- Frame size varies like Opus speech at about 16–24 kbit/s: roughly 40–80
  bytes, from a seeded PRNG so runs are repeatable. Allow a fixed-size mode
  too.
- Payload: marker, sequence number, sender timestamp (µs), then filler.
- Sealed as type 2 (voice) under the stage 4 session key and sent unicast to
  board 2.

### Simulated speaker (receiver, board 2)

- Records the arrival time of every voice frame and runs a playout schedule:
  a jitter buffer of configurable depth (default 60 ms) with one 20 ms slot
  per sequence number from the first frame. Each frame is **on time**,
  **late** (arrived after its slot) or **lost** (never arrived).
- Reports inter-arrival jitter (no clock sync needed) and on-time, late and
  lost counts.

### Delay

The two boards' clocks are not synchronised. Measure round-trip time as in
stage 3. For example, board 2 answers every Nth voice frame (say every 5th)
with a small echo frame carrying its sequence number. Board 1 then times
send to echo-received, and one-way delay is estimated as RTT/2. Document
the method and its assumption (symmetric path).

### Crypto cost

Time `seal` on board 1 and `open` (including the replay window) on board 2
with `esp_timer_get_time()` for every frame. Report min / median / p99 /
max, and as a share of the 20 ms frame interval.

### Builds

| Environment | Board | Purpose |
| --- | --- | --- |
| `board1`, `board2` | 1, 2 | Encrypted voice sender / receiver, also sending text |
| `board1-plain`, `board2-plain` | 1, 2 | Encryption off: confidentiality control and latency baseline |
| `board3-sniff` | 3 | Passive sniffer, no keys |

The relay comparison is **optional**: only if time allows and the user
agrees, port the stage 3 relay (`e2e_relay_test/src/relay.cpp`) to this
project and add a relayed voice run.

### Tests

| # | Test | Pass (proposed targets; report actual figures either way) |
| --- | --- | --- |
| V1 | 60 s encrypted voice, 50 fps | Crypto per frame (seal + open) under 2 ms (10% of the frame interval); late + lost under 1% with a 60 ms buffer; no queue drops |
| V2 | 60 s plaintext baseline | Same measurements; encryption's added delay reported as V1 − V2 |
| V3 | Jitter buffer sweep | Late + lost at 20, 40, 60, 80 ms buffer depths, from the same arrival log if possible |
| V4 (optional) | Through the relay | Added delay per hop at voice rate, compared with stage 3's 3.2 ms |

Capture C1 at the same time as V1 if serial bandwidth allows; otherwise run
them separately.

## Host tests

Keep the stage 4 host tests passing (`tests/run_host_tests.sh`, OpenSSL 3).
Add host tests for whatever logic is not radio-bound: the voice payload
format, the playout schedule (on time / late / lost classification,
including reordering and gaps), statistics, and the capture analysis on
hand-made inputs. As in earlier stages, mutation-check the important rules
(break them on purpose and confirm a test fails).

## Deliverables

- `tested_parts/e2e_voice_test/` project, building cleanly with no warnings.
- `README.md` in the style of the stage 3 and 4 READMEs: what is reused,
  design, build environments, host tests, hardware procedure with exact
  commands, results with figures quoted from logs, results table, limits,
  project files.
- Logs in `evidence/stage5/`, checked with `tools/find_keys.py` and the
  analysis tool before committing.
- Update the stage table row 5 in `e2e_security_test/README.md`, and the
  project tree and project table in the root `README.md`.

## Out of scope

- Real audio: codec behaviour and sound quality are not measured. Say so in
  the README limits.
- LoRa, and passive capture of anything other than ESP-NOW on channel 1.
- Traffic analysis (frame sizes and timing reveal that someone is talking,
  and roughly how much). Note it as a limit; frame-size padding could be a
  later change.

## Decisions to confirm with the user early

- Jitter buffer default (60 ms proposed) and the pass targets above.
- Whether the relay run (V4) is in scope.
- Sniffer output: high baud with full hex, or sampled hex plus summaries.
