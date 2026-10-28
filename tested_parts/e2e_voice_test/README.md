# Stage 5: confidentiality capture and synthetic voice

Hardware tests C1–C3 and the encrypted V1 targets passed on 2026-10-27.
The plaintext baseline V2 and buffer sweep V3 are complete. Plaintext
lateness was 1.6% at 60 ms, above the encrypted run’s 0.5%; both had zero
loss and queue drops. The optional relay run V4 was not performed. [SPEC.md](SPEC.md) defines the experiment.

## Design and reuse

Copied byte for byte from Stage 4: `secure_message`, `replay_window`,
`crypto_backend`, `kdf`, `control_frame`, `pairing`, `session`, `radio`,
their host tests and runner, `capture_logs.py`, `find_keys.py`,
`generate_keys.py`, `partitions.csv`, and `.gitignore` (26 files checked).
The handset application retains Stage 4 serial/button pairing and session
handshakes. NVS remains `pairs` / `peer<N>` with 16-byte pair keys, so the
existing pairing survives. A fresh gitignored network key was generated
for this project; both handsets must use builds from this same checkout.

Board 1 sends `HELLO SE19 #n` once per second. The `voice` command starts
one 3,000-frame experiment per boot: an `esp_timer` callback captures a
microsecond timestamp and queues a sequence number every 20 ms. Encryption,
radio transmission and logging run outside the callback. A seeded
xorshift32 selects 40–80-byte payloads; `voice fixed` uses 60 bytes.
A full timer queue increments a drop count and leaves a sequence gap.

Voice payload: `SE19VOICE` (9 bytes), sequence (4), total frames (4),
capture timestamp (8), then repeating `SE19FILL`. Integers are little
endian. This body uses the unchanged type-2 session-encrypted frame with
40 bytes of protection overhead. The receiver answers every fifth frame
with an equal-sized `SE19ECHO` payload, sequence and filler.

The sender measures capture-to-echo RTT, including timer queueing, crypto,
radio and application processing; RTT/2 estimates one-way delay under a
symmetric-path assumption. No cross-board timestamp subtraction is used
for absolute delay. Board 2 records arrival at application dequeue before
opening the frame, so receiver queue delay is included; this is not an
RF-interrupt timestamp. Open timing includes the session replay check.
The inherited dispatch also verifies the hop tag before `open`, so its
additional verification cost is in RTT but outside the `open` timer.

The first received sequence anchors playout. Sequence `s` has slot
`first_arrival + depth + (s - first_sequence) * 20 ms`. Arrivals on the
boundary are on time, later arrivals are late, and absent sequences are
lost. Leading/trailing missing frames count toward the 3,000-frame total.
Results print two seconds after the expected end; reset both endpoints
before another experiment. `report` during a run includes future frames
as missing and is therefore provisional. Arrival logs support the same
20/40/60/80 ms sweep and RFC 3550 interarrival jitter calculation offline.

`board1-plain` and `board2-plain` are explicitly labelled **negative
controls**. Data frames retain the header and length, with clear payload
and zero-filled tag slots; no data authentication or replay protection is
claimed. Pair/session control traffic retains Stage 4 handling. The
plaintext data path works even without an established session.

The sniffer compiles only `sniffer.cpp` and `sniff_frame.cpp`, includes no
key header, crypto or session code, and never transmits application frames.
It captures management action frames on channel 1, checks Espressif's OUI,
type 4 and ESP-NOW v1 element, strips the FCS, and filters both directions
between boards 1 and 2. Each `CAP` line contains local time, source and
destination MAC, payload length and hex. All boards use **921600 baud**.
Queue overflow, serial backlog and malformed endpoint management frames
have separate counters. Unrelated traffic is filtered rather than counted
as capture loss. Retransmissions are retained for analysis.

## Builds and host checks

Run from this directory:

```sh
pio run
sh tests/run_host_tests.sh
sh tests/run_voice_tests.sh
python3 tests/mutation_check.py
```

| Environment | Board | Function |
| --- | --- | --- |
| `board1` | 1 | Encrypted sender and pairing |
| `board2` | 2 | Encrypted receiver and echo |
| `board1-plain` | 1 | Clear sender, baseline |
| `board2-plain` | 2 | Clear receiver, baseline |
| `board3-sniff` | 3 | Keyless passive capture |

Validation on 2026-10-27: five builds succeed without warnings; inherited
secure-message, pairing and session suites pass; voice/parser tests and
four Python analysis tests pass. Eight temporary-copy mutations are killed
(payload range, loss, playout depth, echo duplicates, percentile, vendor
type, repeated ciphertext, capture completeness). Validation output is in
[evidence/stage5/validation.txt](evidence/stage5/validation.txt).

## Manual hardware procedure

The spec reserves flashing to the user. **Do not automate uploads.** Enter
the bootloader with BOOT + RESET as needed, then run these commands from
this directory, one board at a time:

```sh
pio run -e board1 -t upload --upload-port /dev/cu.usbmodem5B420192981
pio run -e board2 -t upload --upload-port /dev/cu.usbmodem5B5F0215971
pio run -e board3-sniff -t upload --upload-port /dev/cu.usbmodem5B420192791
```

Confirm ports with `pio device list`. Close other serial monitors. Opening
each port resets that board. `capture_stage5.py` wraps the unchanged Stage 4
capture utility solely to select `evidence/stage5/`.

After flashing, capture C1 and V1 together:

```sh
~/.platformio/penv/bin/python tools/capture_stage5.py encrypted \
  sniffer=/dev/cu.usbmodem5B420192791 \
  receiver=/dev/cu.usbmodem5B5F0215971@3 \
  sender=/dev/cu.usbmodem5B420192981@6 \
  --baud 921600 --duration 80 --send sender@12=voice \
  --send sender@76=report --send receiver@76=report
```

Require `SESSION established`, `VOICE_START 3000 1`, final drop summaries
and receiver playout output. If `VOICE` refuses because no session exists,
check `keys`. If necessary send `pair 2` to board 1 and `pair 1` to board 2;
compare their six-digit codes and send `yes` to both only when equal.
Then repeat the capture with a new run name. Do not erase NVS unnecessarily.

For C2/V2, manually flash the two negative controls (keep board 3):

```sh
pio run -e board1-plain -t upload --upload-port /dev/cu.usbmodem5B420192981
pio run -e board2-plain -t upload --upload-port /dev/cu.usbmodem5B5F0215971
```

Repeat the capture command with run name `plain`. Each capture opens and
resets both endpoints; this guarantees a fresh experiment.

## Analysis and evidence

```sh
python3 tools/analyse_capture.py \
  --sender evidence/stage5/plain_sender.txt \
  --receiver evidence/stage5/plain_receiver.txt \
  --sniffer evidence/stage5/plain_sniffer.txt --plain \
  > evidence/stage5/plain_analysis.json
python3 tools/analyse_capture.py \
  --sender evidence/stage5/encrypted_sender.txt \
  --receiver evidence/stage5/encrypted_receiver.txt \
  --sniffer evidence/stage5/encrypted_sniffer.txt \
  --baseline evidence/stage5/plain_analysis.json \
  > evidence/stage5/encrypted_analysis.json
python3 tools/find_keys.py .pio/build/board3-sniff/firmware.bin \
  evidence/stage5/*.txt evidence/stage5/*.json
```

Completeness compares exact unique captured frames against successful
local queue submissions in both endpoint logs; local queue acceptance is
not proof of RF delivery. Marker searches cover every captured data frame,
including echoes. The tool reports retransmissions, readable header
(type/source/destination/counter) ranges, conflicting frames with identical
headers, and repeated encryption of the fixed voice marker at different
counters. It compares the invariant marker region because the complete
voice plaintext changes with sequence and timestamp. Empty captures fail.
Use one boot per log; do not concatenate separate experiments.

Latency output gives min/lower-median/nearest-rank p99/max for seal, open,
RTT and RTT/2; crypto as percent of 20 ms; jitter; four playout depths;
and encrypted minus plaintext median estimated delay. The sum of maximum
seal and maximum open is a conservative per-frame crypto bound, not a
matched-frame percentile. Inspect final drop counters and send errors as
well as distributions. The tool's exit code covers confidentiality only;
V1 targets require complete run evidence and review of these metrics.

`find_keys.py` searches the generated network key and reports matches; its
exit status does not enforce absence. Verify the text says `no keys`.
It cannot search pair/session keys held only in NVS/RAM, which are never
logged. Source isolation separately establishes that the sniffer has no
pair/session key implementation. Scan every evidence file before committing.

| Test | Target | Result |
| --- | --- | --- |
| C1 | ≥95% capture, no markers, distinct marker ciphertext | Pass: 3,674/3,674 unique frames; zero marker hits or repeated marker ciphertext |
| C2 | Markers in every captured data frame | Pass: 100% capture completeness and marker visibility |
| C3 | No key in sniffer image or logs | Pass for generated network key: image, all logs and analysis JSON |
| V1 | Crypto <2 ms; late + lost <1% at 60 ms; zero queue drops | Pass: 1.471 ms conservative crypto bound; 0.5% late, zero lost/drops |
| V2 | 60 s plaintext baseline and V1 − V2 | Complete: +2.408 ms encrypted median RTT/2; baseline 1.6% late at 60 ms |
| V3 | Buffer sweep at 20/40/60/80 ms | Complete: encrypted late 160/55/15/5; plaintext 634/164/48/14; zero lost |
| V4 | Optional relay comparison | Out of scope for this run |

## Encrypted hardware results, 2026-10-27

Evidence: `encrypted_sender.txt`, `encrypted_receiver.txt`,
`encrypted_sniffer.txt`, and `encrypted_analysis.json` under
[evidence/stage5/](evidence/stage5/). Capture lasted 80 seconds; the voice
command ran at 12 seconds, with 3,000 scheduled frames over 60 seconds.
Both endpoints retained pair fingerprint `48FF15EF` and established
session `0390FD02`. All 600 sampled echoes returned.

The sniffer matched all 3,674 unique queued data frames (3,000 voice,
600 echoes, 74 text), plus 35 duplicate data captures consistent with
retransmissions. All four marker searches returned zero. No conflicting
same-header frames or repeated encrypted marker regions were found.
Sniffer queue/serial drops and malformed counts were zero in all summaries;
endpoint timer/radio queue drops were zero. `find_keys.py` reported no
network key in the sniffer image, any encrypted log, or the analysis JSON.

Crypto times in microseconds (min / median / p99 / max): seal
469 / 621 / 712 / 823; open 399 / 457 / 559 / 648, each over 3,000 frames.
The sum of maxima is 1,471 µs, or 7.355% of a 20 ms interval.
RTT over 600 echoes was 8,713 / 30,720 / 107,785 / 136,744 µs.
Estimated one-way median is 15.36 ms and p99 is 53.893 ms, subject to the
symmetric-path assumption. The plaintext comparison below reports the observed delay difference.

At buffer depths 20/40/60/80 ms, late counts were 160/55/15/5 of 3,000
(5.333% / 1.833% / 0.500% / 0.167%). No frames were lost. Final RFC 3550
jitter was 9.429 ms. The offline sweep agrees with firmware `PLAYOUT`
output, including 2,985 on-time and 15 late at the default 60 ms depth.

## Plaintext baseline and comparison, 2026-10-27

Evidence: `plain_sender.txt`, `plain_receiver.txt`, `plain_sniffer.txt`,
and `plain_analysis.json` under `evidence/stage5/`. Both endpoints logged
`PLAINTEXT NEGATIVE CONTROL`. The same 80-second capture and 60-second,
3,000-frame voice experiment ran after manual flashing. All 3,000 voice
frames and 600 echoes arrived, with zero timer/radio/sniffer queue drops,
zero serial drops and no malformed frames in sniffer summaries.

All 3,674 unique queued data frames were captured. Including 26 duplicate
data captures, marker hits were `HELLO SE19`: 74; `SE19VOICE`: 3,001;
`SE19ECHO`: 625; and `SE19FILL`: 3,626. Every captured data frame contained
a marker. Repeated marker regions are expected in this cleartext control;
the analysis field named `repeated_marker_ciphertext_across_counters`
therefore does not represent ciphertext or a failure for this build.
The image, all six capture logs and both analysis JSON files were scanned
with `find_keys.py`; all reported `no keys` for the generated network key.

Plaintext framing/copy timing in microseconds (min / median / p99 / max)
was 1 / 2 / 5 / 7 for send and 1 / 1 / 2 / 3 for receive. These are not
cryptographic operations. RTT was 5,925 / 25,905 / 89,903 / 159,950 µs
across 600 echoes; estimated one-way median was 12.9525 ms and p99 was
44.9515 ms. Encrypted minus plaintext median RTT/2 is **+2.4075 ms**;
the corresponding p99 difference is +8.941 ms. The sum of median timed
seal/open costs differs by 1.075 ms (1.078 ms encrypted versus 0.003 ms
plaintext); this is distinct from the full path delay comparison.

| Buffer depth | Encrypted late / 3,000 | Plaintext late / 3,000 |
| --- | --- | --- |
| 20 ms | 160 (5.333%) | 634 (21.133%) |
| 40 ms | 55 (1.833%) | 164 (5.467%) |
| 60 ms | 15 (0.500%) | 48 (1.600%) |
| 80 ms | 5 (0.167%) | 14 (0.467%) |

Both runs had zero loss at every depth. Final baseline RFC 3550 jitter was
10.663 ms, versus 9.429 ms encrypted. Offline counts match the firmware
reports. The baseline exceeds 1% lateness at 60 ms, while encrypted V1
meets its proposed target. Do not interpret the encrypted run's lower
lateness as an encryption benefit: the experiments ran separately under
variable radio/scheduler conditions, and each playout schedule anchors to
its own first arrival. Likewise, the observed RTT/2 difference is not a
controlled estimate of crypto cost alone. These are single-run results,
not confidence intervals or a guarantee of sustained audio performance.

The boards were left running the plaintext controls after this experiment;
restore `board1` and `board2` manually before using protected traffic.
No upload, commit or push was performed by the agent.

## Limits and files

This measures synthetic framing, not a codec, microphone, speaker or sound
quality. It covers ESP-NOW v1 on channel 1, not LoRa. Sizes, headers and
traffic timing remain visible. Finite marker tests are evidence of correct
confidentiality behaviour, not a cryptographic proof. Logging and scheduler
load affect latency, and oscillator drift affects the playout schedule.
The sender/receiver roles and experiment length are fixed; reset both
between runs. The default reporting depth is 60 ms, with other depths
computed from the same observations. Padding and relay testing are future
work.

- `src/main.cpp`, `src/voice_app.inc`: pairing integration and experiment.
- `include/voice.h`, `src/voice.cpp`: payloads, sizing, playout, statistics.
- `src/sniffer.cpp`, `src/sniff_frame.cpp`: keyless capture and parser.
- `tests/`: inherited suites, voice/parser, analysis and mutation tests.
- `tools/analyse_capture.py`: confidentiality and latency reporting.
- `tools/capture_stage5.py`: output-path adapter for inherited serial tool.
- `evidence/stage5/`: validation, encrypted/plaintext hardware captures and analysis.
