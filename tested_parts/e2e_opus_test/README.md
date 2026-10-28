# Stage 6: Opus voice over the end-to-end link

Hardware runs finished on 2026-10-28, on channel 11. Real Opus packets
crossed the encrypted link bit for bit: 3,000 of 3,000, every CRC32
matching. Decoded in order, they match the source at 0.9989 correlation
and 26.73 dB SNR. The sender and receiver pipelines kept up, with the
slowest step at 8.6 ms. No Opus packet or marker appeared on the air.

O4 failed. With a 60 ms jitter buffer, 2.87% of frames were late. The
delay is not in the firmware: the sender's radio held frames before
transmitting, with or without encryption (see *Where the delay is*).
[SPEC.md](SPEC.md) defines the experiment.

## Design and reuse

Built from Stage 5 (`e2e_voice_test`). These 34 files are byte-identical
to Stage 5: `secure_message`, `replay_window`, `crypto_backend`, `kdf`,
`control_frame`, `pairing`, `session`, `sniffer`, `sniff_frame`, `voice`,
their headers and host tests, `run_host_tests.sh`, `capture_logs.py`,
`find_keys.py`, `generate_keys.py`, `partitions.csv` and `.gitignore`.
Two inherited files changed:

- `src/radio.cpp` turns Wi-Fi power save off (`esp_wifi_set_ps(
  WIFI_PS_NONE)`) and logs `Wi-Fi power save: off`. Arduino 2.0.17
  defaults to modem sleep on the ESP32-S3, and the handset radio code from
  Stage 1 onwards never turned it off (see *Diagnosis*).
- `include/network_config.h` uses channel 11 instead of 1.

`codec.h` / `codec.cpp` come from `codec_sdtest` with the same library
commit (`arduino-libopus` `bae0f85`). The only change is one added
function, `codec_conceal`, which calls `opus_decode` with a null packet for
packet loss concealment. Nothing was removed.

NVS keeps the Stage 4 format (`pairs` / `peer<N>`), so the boards needed
no re-pairing (pair key `48FF15EF`). This project has its own gitignored
network key, so all three handset builds must come from this checkout.

### Opus settings and test signal

From `codec_test`, unchanged: 16 kHz mono PCM16, Audio application,
24 kbit/s CBR, 20 ms frames (320 samples), complexity 5, DTX off. CBR gives
60-byte packets. CBR also protects confidentiality. With VBR, packet sizes
follow the speech, and published attacks have recovered phrases from
encrypted VBR calls by size alone. With CBR every packet is the same size.

`opus_voice::test_sample` generates a 3 s pattern repeated for 60 s:
440 Hz and 1 kHz tones for 1 s, a 300 Hz to 2.1 kHz sweep for 1.5 s, and
0.5 s of silence. It uses integer arithmetic only, so the boards and the
host produce identical samples, and phase stays continuous across frames.
Board 2 regenerates the reference locally; no audio crosses the serial
port.

### Sender (board 1)

An `esp_timer` fires every 20 ms and queues a capture timestamp and
sequence number. It does no other work. One codec task generates and
encodes each frame. It is the only task that calls Opus, because the
library is not thread safe. It has a 64 KiB stack and runs on core 0 at
priority 1, below Wi-Fi. The loop on core 1 builds the payload, seals it
as type 2, sends it and logs it, so session state stays in one task.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | Marker `SE19OPUS` |
| 8 | 4 | Sequence number, little endian |
| 12 | 4 | Frames in the stream (3,000) |
| 16 | 8 | Capture time, sender µs |
| 24 | 2 | Opus packet length |
| 26 | 60 | Opus packet |

The 86-byte payload makes a 126-byte frame on the air. `OPUS_TX` logs the
sequence, packet length, CRC32, generate, encode and capture-to-send
times, the send result and the packet hex. `HELLO SE19 #n` still goes out
once per second. Echo round trips start just before sealing, so RTT covers
seal, radio, open and the echo's return, not the codec.

### Receiver (board 2)

The loop opens each frame and stamps its arrival when it dequeues it, as
in Stage 5. It stores the packet in PSRAM and logs `ARRIVAL` and `OPUS_RX`
(sequence, length, CRC32). Every fifth frame gets an equal-sized Stage 5
echo. The codec task plays each 20 ms slot 2 ms after it is due, so a
frame stamped just before its slot has been stored. On time still means
arrival stamp ≤ slot. It decodes on-time frames and conceals late or
missing ones. Its output is the audio a speaker would have played, and it
is compared with the reference after skipping the encoder's 104-sample
lookahead.

After the stream, the player replays the stored packets with a fresh
decoder at 20/40/60/80 ms and in sequence order, ignoring timing. The
60 ms replay must equal the live audio (`LIVE_MISSED 0`). The in-order
figure shows what Opus itself costs. The gap between it and the timed
figures shows what late frames cost.

### Plaintext control and sniffer

`board1-plain` / `board2-plain` send the same payloads in clear with
zero-filled tag slots. They are **negative controls** and the latency
baseline, with no data protection claimed. `board3-sniff` is the
unchanged Stage 5 sniffer, which prints `Stage 5 passive sniffer`. It
compiles only `sniffer.cpp` and `sniff_frame.cpp`, has no key, crypto or
session code, and never transmits. It now listens on channel 11, through
`network_config.h`.

## Builds and host checks

Run from this directory:

```sh
pio run
sh tests/run_host_tests.sh
sh tests/run_opus_tests.sh
python3 tests/mutation_check.py
```

The Opus host tests use the system libopus (`brew install opus`; 1.6.1
here) for a real encode and decode round trip.

| Environment | Board | Function |
| --- | --- | --- |
| `board1` | 1 | Encrypted Opus sender and pairing |
| `board2` | 2 | Encrypted receiver, player and echo |
| `board1-plain` | 1 | Clear sender, negative control |
| `board2-plain` | 2 | Clear receiver, negative control |
| `board3-sniff` | 3 | Keyless passive capture |

Validation on 2026-10-28, on the final code:
- The five environments build cleanly with no warnings.
- The inherited secure-message, pairing and session suites pass.
- The Opus suite passes: test signal, payload format, CRC32, lookahead
  alignment, SNR and correlation, decode and conceal, replay.
- The voice, sniffer and 11 Python analysis tests pass.
- All 16 temporary-copy mutations are killed. They cover playout, payload
  length, lookahead, CRC, the test signal, concealment, replay, packet
  search, integrity, loss, vendor type, repeated ciphertext, completeness
  and counter reuse.

Output is in
[evidence/stage6/validation.txt](evidence/stage6/validation.txt).

## Manual hardware procedure

The user flashes every board. **Do not automate uploads.** Enter the
bootloader with BOOT + RESET as needed, then, from this directory:

```sh
pio run -e board1 -t upload --upload-port /dev/cu.usbmodem5B420192981
pio run -e board2 -t upload --upload-port /dev/cu.usbmodem5B5F0215971
pio run -e board3-sniff -t upload --upload-port /dev/cu.usbmodem5B420192791
```

Board 3 must be flashed from this project too, or it listens on the old
channel. Confirm ports with `pio device list` and close other monitors.
Opening a port resets that board. Keep the Mac awake with `caffeinate`:
one capture was lost when the Mac idle-slept on battery. The playback and
replays end about 200 s in, so capture for 220 s:

```sh
caffeinate -dims ~/.platformio/penv/bin/python tools/capture_stage6.py \
  encrypted \
  sniffer=/dev/cu.usbmodem5B420192791 \
  receiver=/dev/cu.usbmodem5B5F0215971@3 \
  sender=/dev/cu.usbmodem5B420192981@6 \
  --baud 921600 --duration 220 --send sender@12=voice \
  --send sender@210=report --send receiver@210=report
```

Check for `SESSION established`, `VOICE_START 3000 1`, `VOICE_DROPS` from
both ends and the receiver's `QUALITY` lines. `QUALITY pending` means the
capture ended too early. For the control, flash `board1-plain` and
`board2-plain` (keep board 3) and repeat with run name `plain`.

## Analysis and evidence

```sh
python3 tools/analyse_capture.py \
  --sender evidence/stage6/plain_sender.txt \
  --receiver evidence/stage6/plain_receiver.txt \
  --sniffer evidence/stage6/plain_sniffer.txt --plain \
  > evidence/stage6/plain_analysis.json
python3 tools/analyse_capture.py \
  --sender evidence/stage6/encrypted_sender.txt \
  --receiver evidence/stage6/encrypted_receiver.txt \
  --sniffer evidence/stage6/encrypted_sniffer.txt \
  --baseline evidence/stage6/plain_analysis.json \
  > evidence/stage6/encrypted_analysis.json
python3 tools/air_delay.py evidence/stage6/encrypted \
  > evidence/stage6/encrypted_air_delay.json
python3 tools/air_delay.py evidence/stage6/plain \
  > evidence/stage6/plain_air_delay.json
python3 tools/find_keys.py .pio/build/board3-sniff/firmware.bin \
  evidence/stage6/*.txt evidence/stage6/*.json \
  evidence/stage6/channel1/*.* evidence/stage6/channel1/modem_sleep/*.txt
```

`analyse_capture.py` extends Stage 5's tool with three checks:
- **Packet integrity:** every sender CRC32 has a matching receiver CRC32.
- **Opus on the air:** every captured payload is searched for every sent
  Opus packet's bytes, taken from the sender's hex log.
- **Timing:** per-step distributions and the mouth-to-ear estimate.

One inherited check changed. A header (type, source, destination,
counter) seen with two different frames means a counter was reused under
one key. Stage 5 applied that check to every captured frame. Stage 6
applies it to the frames this boot's two endpoints logged. Frames left on
the air by an earlier boot were sealed under older session keys. They are
now counted as `unlogged_frames` instead of failing the run. In the first
passing capture, five such frames had tripped the old check. A test and
two mutations cover both cases.

`air_delay.py` splits each voice frame's one-way delay at the sniffer's
air timestamp:
- **send → air:** from the sender's `esp_now_send` to the frame on the
  air.
- **air → loop:** from the air to the receiver's dequeue.

The three boards' clocks share no offset, so each figure is relative to
the fastest frame.

The analysis tool's exit code covers confidentiality only. `find_keys.py`
searches for the generated network key; check that it prints `no keys`
for every file. It reported `no keys` for the sniffer image and every
Stage 6 log and JSON file.

## Results

| Test | Target | Result |
| --- | --- | --- |
| O1 | All 3,000 packets, every CRC32 matches | **Pass:** 3,000 / 3,000 sent, received and matching |
| O2 | In-order decode: correlation > 0.99, SNR > 20 dB | **Pass:** 0.9989 / 26.73 dB, worst 1 s window 25.04 dB |
| O3 | No step over 20 ms; capture-to-send p99 < 15 ms; zero drops | **Pass:** worst step 8.6 ms (encode); capture-to-send p99 9.0 ms; zero timer, radio, encode, codec and send drops |
| O4 | Late + lost < 1% at 60 ms | **Fail:** 86 late (2.87%), 0 lost; live 0.9797 / 13.93 dB, worst window 6.50 dB |
| O5 | Mouth-to-ear median < 150 ms | **Pass:** median 110.1 ms, p99 146.1 ms |
| O6 | Sniffer ≥ 95%; no Opus packets or `SE19OPUS` on the air | **Pass:** 3,814 / 3,814 frames; 0 packets in 3,044 voice captures; 0 markers |
| O7 | Plaintext: Opus packets and markers in every frame | **Pass:** all 3,032 voice captures carry a sent packet; all 130 distinct packets seen; markers in 100% of frames |
| O8 | No key in sniffer image or logs | **Pass:** `no keys` for the image, every log and JSON |
| O9 | O4's figures at 20/40/60/80 ms | **Complete:** see the sweep table |

### Encrypted run, channel 11

Evidence: `encrypted_*` under [evidence/stage6/](evidence/stage6/). Session
`4834EAE8`. All 3,000 frames were sent with zero send errors, and all 600
sampled echoes returned. The sniffer captured all 3,814 unique data frames
(3,000 voice, 600 echoes, 214 text) and 44 retransmissions. Its queue,
serial and malformed counters stayed at zero.

| Step (µs) | Min | Median | p99 | Max |
| --- | --- | --- | --- | --- |
| Generate | 56 | 470 | 797 | 992 |
| Encode | 2,862 | 7,042 | 7,958 | 8,559 |
| Capture to send | 3,716 | 7,758 | 9,001 | 9,914 |
| Seal | 496 | 661 | 775 | 876 |
| Open | 412 | 480 | 798 | 901 |
| Decode (2,914) | 1,905 | 2,569 | 4,116 | 5,151 |
| Conceal (86) | 1,390 | 5,041 | 7,389 | 7,389 |
| Play lag after slot | 2,137 | 2,143 | 2,338 | 2,758 |
| RTT (600 echoes) | 11,015 | 35,219 | 101,266 | 132,050 |

Seal and open take 1.14 ms at the median. The sum of their maxima is
1.777 ms, 8.9% of a frame. RFC 3550 jitter was 19.1 ms.

Mouth-to-ear estimate = 20 ms frame + capture-to-send + RTT/2 + buffer
depth + play lag + decode. Capture-to-send covers generate, encode and
queueing. RTT/2 covers seal, radio and open; it assumes a symmetric path,
and the sniffer shows the path is not symmetric. The terms do not
overlap: both capture-to-send and RTT stop or start just before sealing.

- **Medians:** 20 + 7.8 + 17.6 + 60 + 2.1 + 2.6 = **110.1 ms**.
- **p99s:** 20 + 9.0 + 50.6 + 60 + 2.3 + 4.1 = **146.1 ms**, a sum of
  per-term p99s rather than a measured p99.

Both are under ITU-T G.114's 150 ms. At an 80 ms buffer the median would
be about 130 ms.

| Buffer | Late (of 3,000) | Live audio corr / SNR | Worst 1 s window |
| --- | --- | --- | --- |
| 20 ms | 1,131 (37.7%) | 0.7278 / 3.18 dB | 1.34 dB |
| 40 ms | 402 (13.4%) | 0.9120 / 7.69 dB | 2.80 dB |
| 60 ms | 86 (2.87%) | 0.9797 / 13.93 dB | 6.50 dB |
| 80 ms | 5 (0.17%) | 0.9988 / 26.07 dB | 18.87 dB |
| In order | 0 | 0.9989 / 26.73 dB | 25.04 dB |

No frames were lost at any depth. The live 60 ms audio equals the 60 ms
replay. Each concealed frame costs a lot of SNR, because the test signal's
tones and sweep don't extrapolate well. That makes SNR a harsh measure for
speech; it is not a listening test.

### Plaintext control, channel 11

Evidence: `plain_*`. Both endpoints logged `PLAINTEXT NEGATIVE CONTROL`.
All 3,000 frames were sent with zero errors, received and CRC-matched, and
all 3,814 unique frames were captured. Every captured voice frame
contained a sent Opus packet (3,032 of 3,032), and all 130 distinct
packets in the signal appeared. Marker hits: `HELLO SE19` 214, `SE19OPUS`
3,032, `SE19ECHO` 600, `SE19FILL` 600. The repeated-marker field counts
3,598, which is expected for cleartext and is not a failure here.

Clear framing took 4 µs to send and 2 µs to receive (medians). RTT was
10,059 / 31,123 / 121,303 / 211,153 µs. Late frames were 513 / 136 / 63 /
46 at 20/40/60/80 ms, so even the plaintext run fails O4 at 60 ms, and it
fails at 80 ms too (1.53%). Encrypted minus plaintext median RTT/2 is
+2.05 ms, close to the 1.14 ms of median seal and open. As in Stage 5,
these are separate single runs, not a controlled measure of crypto cost.

### Where the delay is

| Run | send → air median / p99 | air → loop median / p99 |
| --- | --- | --- |
| Channel 11, encrypted | 12.7 / 71.3 ms | 0.8 / 7.5 ms |
| Channel 11, plaintext | 15.0 / 120.0 ms | 0.7 / 5.7 ms |
| Channel 1, encrypted | 5.7 / 45.5 ms | 0.8 / 6.3 ms |
| Channel 1, encrypted repeat | 9.5 / 74.6 ms | 0.6 / 7.1 ms |

Once a frame is on the air, the receiver has it within a millisecond. The
variable delay is between the sender calling `esp_now_send` and the frame
leaving its radio, and plaintext frames wait just as long. Load doesn't
explain it either: frames sent during the signal's silence (2.8 ms
encodes) waited as long as frames during the tones (7 ms), at a median of
12.3 vs 12.8 ms. That points to the radio waiting for a clear channel.
ESP-NOW frames are ordinary 802.11 frames and share airtime with every
nearby network: the Mac saw 4–5 access points on channel 1, and one on
channel 11.

This is inferred, not measured. Channel occupancy was not recorded, and
Bluetooth and other 2.4 GHz devices are also candidates.

### Diagnosis and earlier runs

These runs are kept in `evidence/stage6/channel1/`, with the same tools'
output beside them.

1. **Modem sleep** (`channel1/modem_sleep/`): the first run had 1,167
   `ESP_ERR_ESPNOW_NO_MEM` send errors (the ESP-NOW send queue was full).
   RTT grew from 0.2 s to 1.2 s, and 1,817 frames were late. The sniffer
   saw frames leave in 2 ms bursts, on a 102.4 ms phase: the Wi-Fi power
   save beacon interval. Stage 5's capture shows the same phase with
   gaps up to 107 ms, but Stage 5 had spare capacity. Turning power save
   off fixed the steady backlog.
2. **Channel 1 after the fix** (three partial runs; logs not kept): the
   same firmware gave 355 send errors in the first, about 60% errors over
   the first 900 frames in the second, and none in the third.
   Retransmissions on the air stayed rare. The second run's logs ended
   early because the Mac slept.
3. **Channel 1 full runs:**

| Run | Send errors | Late at 60 ms | Late (+ lost) at 80 ms | Live 60 ms corr / SNR |
| --- | --- | --- | --- | --- |
| `encrypted` | 0 | 3 (0.10%) | 1 (0.03%) | 0.9984 / 24.95 dB |
| `encrypted_repeat` | 0 | 45 (1.50%) | 22 (0.73%) | 0.9888 / 16.53 dB |
| `plain` | 88 | 253 (8.43%) + 88 lost | 239 (7.97%) + 88 lost | 0.9382 / 9.22 dB |

The channel 1 plaintext run lost its frames in one 10 s stall. Its
integrity check counts 2,912 of 3,000 because 88 were never sent; all
2,912 sent packets matched. Every channel 1 run passed O1 and its
confidentiality checks. Stage 5's latency figures were probably affected
by modem sleep too.

## Limits

- **Single runs in an uncontrolled 2.4 GHz environment.** Of the five
  valid encrypted runs after the modem sleep fix, O4's 60 ms target was
  met in two (channel 1 `encrypted` and the third partial run). The
  firmware's own timing was stable across every run: encode p99
  7.8–8.0 ms, capture-to-send p99 8.8–9.1 ms.
- **The cause of the radio hold is inferred.** Measuring channel
  occupancy, or testing in a quieter place, would confirm it. A handset
  in this environment would need an 80 ms buffer or more, or an adaptive
  buffer.
- **Audio is a generated signal.** There is no microphone, speaker, PESQ
  or listening test. SNR overstates the cost of concealment for speech.
- **Clocks.** RTT/2 assumes a symmetric path. Arrival is stamped at
  application dequeue, and the playout schedule anchors to the first
  arrival, so oscillator drift is not corrected.
- **Metadata stays visible.** Sizes, headers, counters and timing are in
  clear. CBR keeps packet sizes from following the speech.
- **Out of scope:** LoRa, relays, FEC, VBR with padding, and SD storage.

After the runs, the user reflashed `board1` and `board2`. A boot check
showed `Build mode: encrypted` on channel 11 with session establishment,
and `board3-sniff` on channel 11. The agent did no uploads.

## Files

- `src/main.cpp`, `src/opus_app.inc`: Stage 4 pairing plus the Opus
  sender, receiver and player tasks.
- `include/opus_voice.h`, `src/opus_voice.cpp`: payload, test signal,
  CRC32, playout rule and audio comparison.
- `include/opus_player.h`, `src/opus_player.cpp`: decode or conceal, and
  replays.
- `include/codec.h`, `src/codec.cpp`: `codec_sdtest` wrapper plus
  `codec_conceal`.
- `src/radio.cpp`: Stage 5 radio with Wi-Fi power save off.
- `src/sniffer.cpp`, `src/sniff_frame.cpp`: unchanged keyless sniffer.
- `tests/`: inherited suites, Opus host tests, analysis tests, mutations.
- `tools/analyse_capture.py`: integrity, confidentiality, timing, quality.
- `tools/air_delay.py`: send → air and air → loop delay split.
- `tools/capture_stage6.py`: selects `evidence/stage6/` for the inherited
  capture tool.
- `evidence/stage6/`: validation, channel 11 captures and analysis;
  `channel1/` holds the earlier runs and the modem sleep diagnosis.
