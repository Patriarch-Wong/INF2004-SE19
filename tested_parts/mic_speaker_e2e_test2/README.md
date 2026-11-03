# Mic to speaker over the encrypted link, test 2: auto-unmute

A copy of [`mic_speaker_e2e_test`](../mic_speaker_e2e_test/) with one
change: mute is only a serial debug aid. The first frame of any new
incoming stream unmutes the speaker, so a board left muted still plays the
next talker. Everything else (wiring, keys, audio chain, link, commands) is
the same as `mic_speaker_e2e_test`, and so are its keys. The original
stays as it was when it was hardware-tested.

Two boards: one has the MAX9814 microphone, the other the YwRobot speaker.
The mic board encodes speech as Opus and sends it, end-to-end encrypted
(AES-GCM under a per-boot session key), to the speaker board,
which plays it. No screen.

Status: **builds and host tests pass; not yet run on hardware.**

## What changed from `mic_speaker_e2e_test`

| | `mic_speaker_e2e_test` | This project |
| --- | --- | --- |
| `m` mute | Stays muted until `m` again or a reset | Lasts until the next incoming stream |
| New stream while muted | Plays silence | Unmutes, then prints `SPEAKER auto-unmuted for incoming stream` after `RX_START` |

The change is in `on_voice()` in `src/handset.inc`. It does not clear howl
notches or reset the volume when a stream starts; use `r` and `vol N` for
those.

## Wiring (as in ESP32_Mic_Loopback_v4.ino)

| Board | Part | Pins |
| --- | --- | --- |
| Mic board | MAX9814 | VDD → 3V3, GND → GND, OUT → GPIO1 (ADC1 ch 0), GAIN → 3V3 (lowest gain), AR not connected |
| Speaker board | Speaker module | VCC → 5V, GND → GND, SIG ← **GPIO8** (8-bit PWM) |

The speaker stays on GPIO8, the design-review pin; the sketch used GPIO7.
To change it, edit `speaker_pin` in `include/app_config.h`.

Both boards run the same firmware. Each uses whatever is wired to it, so
either board can be the mic board.

## How audio gets through

The sketch was one board doing mic → speaker at 12 kHz. Here its chain is
split across the encrypted link and runs at 16 kHz, Opus's rate. Its
filters are given in Hz, so they are unchanged. Its per-sample smoothing
constants are rescaled so the time constants stay the same
(`audio_dsp.h`).

- Mic (`audio_dsp::voice_gate`, sketch steps 1–2): 4 ADC readings averaged
  per sample (the sketch's oversampling), slow DC tracker, two 300 Hz
  high-pass and two 3 kHz low-pass stages, then the smart noise gate. For
  the first second after boot it measures the room's background noise and
  stays silent, then opens at `noise × ratio + 6` (ratio 3). It holds open
  for 0.3 s after speech, fades in over about 10 ms and out over about
  150 ms, and outputs exactly 0 when closed.
  The sketch busy-waited on `analogRead()`. That would starve the radio
  and the codec, so the ADC's DMA does the readings at an exact 64 kHz
  instead. The averaging is the same.
- Opus 24 kbit/s CBR, 20 ms frames, sealed with the session key.
- Delivery: ESP-NOW unicast retries, a copy of the previous frame in every
  packet, and NACK retransmission within a 300 ms playout buffer.
- Speaker (`audio_dsp::speaker_guard`, sketch steps 3–5): learned notch
  filters, volume on the sketch's 1–32 scale (default 3), and howl
  detection. A loud output lasting 0.12 s gets a notch at its frequency
  and halves the gain, which recovers over about 2 s. Then the tanh soft
  limiter. Output is 8-bit LEDC PWM at 78,125 Hz on GPIO8, idling at 128.
  A 16 kHz hardware-timer interrupt writes one duty value per sample from
  a 64 ms ring buffer.
- Mic gain 1 maps ADC counts × 16 to 16-bit audio, so volume V on the
  speaker board sounds like volume V in the sketch.
- At the end of each stream the speaker board logs `RX_END ... intact=yes`
  only if every frame played and the CRC of all Opus packets matches the
  mic board's `TX_END`.

## Gate strictness

The noise gate sits on the mic board. When it is closed, the board sends
exact silence, so the speaker plays nothing, not even hiss. When it is
open, it passes your voice. It opens when the sound level rises above

```
background noise x strictness + 6
```

The board learns the background noise in the first second after boot and
keeps updating it while the gate is closed. With a background noise of 2:

| Strictness | Opens above | Effect |
| --- | --- | --- |
| 1.5 (lowest) | 9 | Opens easily: quiet speech gets through, but so can fans, typing or hum |
| 3 (default) | 12 | Normal talking opens it; a quiet room keeps it shut |
| 10 (highest) | 26 | Only loud, close speech opens it; soft words or word starts may be cut |

After speech it stays open for 0.3 s and then fades out over about a
second, so the ends of words are not chopped.

On the mic board, type `]` (stricter) or `[` (looser), or `gate R` with R
from 1.5 to 10, then Enter. Raise it if the speaker plays noise when
nobody talks. Lower it if speech cuts in and out. `mic` prints `level`,
`noise` and `open_at` every second. Set the strictness so `open_at` is
above `level` when you are quiet and below it when you speak. The setting
resets to 3 at boot.

## No sound from the speaker?

- `m` toggles mute, a serial debug aid only. The reply line ends in
  `(MUTED)` while muted. Mute lasts until the next incoming stream: its
  first frame unmutes the speaker and prints `SPEAKER auto-unmuted`.
- Volume is the sketch's 1–32 scale (default 3). High volume makes speech
  look like howl: each `SPEAKER howl: notch ...` line cuts a frequency and
  halves the gain. Type `r` to clear the notches and pick a lower `vol N`.
- `tone 1000` plays a test tone that skips the howl guard. If it is
  silent, check the wiring: SIG on GPIO8, VCC on 5V, GND shared with the
  board.

## Build and host tests

```sh
pio run                          # board1, board2
sh tests/run_host_tests.sh       # crypto, session
sh tests/run_handset_tests.sh    # voice link, audio DSP, check_streams
```

Validated 2026-11-02, after the sketch rewrite: both environments build with no warnings (also
with `-Wall -Wextra`), and all suites pass. `tools/find_keys.py` (now
`../shared_keys/find_keys.py`) found only the network key and the 1–2 pair
key in each `firmware.bin`. With the shared file (2026-11-03), each image
also holds that board's unused pair keys for boards 3–5.

## Keys: compiled in, no pairing

As in stages 1–3 (`e2e_security_test`), the keys come from a file, not
from pairing. Since 2026-11-03 that file is the shared
[`../shared_keys/e2e_keys.h`](../shared_keys/README.md), used by every
project that runs on these boards, so a board has the same keys whichever
project it runs. `platformio.ini` adds `../shared_keys` to the include
path. The network key and 1–2 pair key are unchanged from the file this
project committed on 2026-11-02, so boards already flashed keep working.

```sh
python3 ../shared_keys/generate_keys.py --check   # verify the shared file
python3 ../shared_keys/find_keys.py .pio/build/*/firmware.bin
```

The shared file has pair keys for five boards. Each `boardN` build compiles
only its own block: the network key and its 4 pair keys. This project
lists two boards, so it uses only the 1–2 key. It logs `Pair keys compiled
in: 1 in use, 3 for boards not listed or without a MAC`.

The file is **committed on purpose** (at the user's request). This is a
school project. The cost is real: the pair key is the root of the voice
encryption, so anyone with this repository can derive the session keys and
read the voice, as they could with a flash dump. For a private deployment,
see [shared_keys/README.md](../shared_keys/README.md#security). NVS
read-backs, NVS CSVs and core dumps stay gitignored.

At every boot the boards run stage 4's session handshake automatically.
Both derive a fresh session key from the pair key, and voice is sealed
under that. A reboot therefore never reuses an AES-GCM nonce, and frames
recorded before a reboot are rejected after it. Stage 1–3's
receiver-reboot replay gap does not apply here. Boot shows
`Pair keys compiled in: 1`, then `SESSION established with N | session
XXXXXXXX`, with the same session on both boards.

Pair keys from the earlier pairing run are still in NVS on both boards.
This firmware never reads them.

## Flash

**The user flashes the boards.** Board 1 (`E0:72:A1:D8:E2:10`) has the
microphone; board 2 (`E0:72:A1:D8:F6:20`) has the speaker. Both must be
flashed from the same `../shared_keys/e2e_keys.h`.

These boards do not enter download mode by themselves. Before each upload,
hold BOOT, tap RESET, then release BOOT. Press RESET after the upload to
run the new firmware.

```sh
pio run -e board1 -t upload --upload-port /dev/cu.usbmodem5B5F0216001
pio run -e board2 -t upload --upload-port /dev/cu.usbmodem5B420192911
```

Board 1 talks to board 2 and board 2 to board 1. Either can do either;
each uses whatever is wired to it.

## Tests

| # | Test | Where | Pass |
| --- | --- | --- | --- |
| M1 | Mic health: stay quiet for the first second, then read the first 3 `MIC` lines; `mic` for more | Mic board | `adc_hz` 64,000 ± 2%, `invalid 0`, `bias_mv` about 1,250, `gate=closed` and `rms=0` when quiet; speaking gives `gate=OPEN` and `level` above `open_at` |
| M2 | Mic recording: `tools/mic_dump.py` (below), then listen | Mic board | CRC ok, speech clear, silence between words |
| S1 | Speaker alone: `tone 1000`, `tone 300`, `tone 3000`, `sweep`, `+` / `-` | Speaker board | Clean tones, silent before and after, no clicks; `speaker_rate_hz` in `report` is 16,000 |
| E1 | Known signal, encrypted: `test 30` on the mic board | Both | `check_streams.py` passes: 1,500/1,500 played, CRCs match, `intact=yes`; correlation > 0.99, SNR > 20 dB |
| E2 | Live voice: hold BOOT on the mic board (or `talk 10`), speak | Both | Speech intelligible; `intact=yes` |
| E3 | Recovery: E1 with `red off`, boards far apart or blocked | Both | `retransmit` > 0 and still `intact=yes` |
| U1 | Auto-unmute: `m` on the speaker board (reply ends in `(MUTED)`), then `talk 5` on the mic board | Both | Speaker board prints `RX_START`, then `SPEAKER auto-unmuted for incoming stream`, and the speech is heard |

Capture both boards (opening a port resets that board):

```sh
~/.platformio/penv/bin/python tools/capture_logs.py run1 \
    mic=/dev/cu.usbmodem5B5F0216001 spk=/dev/cu.usbmodem5B420192911@1 \
    --duration 60 --send "mic@15=test 30" \
    --send mic@55=report --send spk@55=report
python3 tools/check_streams.py evidence/run1_mic.txt evidence/run1_spk.txt
```

Record the microphone (M2):

```sh
~/.platformio/penv/bin/python tools/mic_dump.py \
    --port /dev/cu.usbmodem5B5F0216001 --seconds 5 evidence/mic_speech.wav
```

## Results

None yet. The earlier hardware results for the same link and audio chain
are in [`mic_speaker_e2e_test`](../mic_speaker_e2e_test/README.md#results-so-far-2026-11-02).

## Commands

| Command | Does |
| --- | --- |
| hold BOOT | Push-to-talk |
| `talk [S]` / `stop` | Talk from serial, optionally for S seconds (max 60) |
| `test [S]` | Send the stage 6 test signal for S s (default 10); the receiver scores it |
| `peer N` | Send to device N |
| `buffer MS` | Playout buffer 60–1000 ms (default 300) |
| `red on` / `red off` | Previous-frame copy in each packet |
| `+` / `-` or `vol N` | Speaker volume, 1–32 (default 3), as in the sketch |
| `]` / `[` or `gate R` | Gate stricter / looser by 0.5, or set it: 1.5–10 (default 3). Use `]` if the mic board passes noise in silence |
| `m` | Debug: mute or unmute the speaker. The next incoming stream unmutes it |
| `r` | Clear the learned howl notches |
| `gain X` | Mic gain (default 1) |
| `mic` | Toggle a `MIC` line every second (`level`, `noise`, gate state) |
| `micdump S` | Record S s of mic audio (use `tools/mic_dump.py`) |
| `tone HZ [MS]` / `sweep` | Speaker test with no codec or radio |
| `report` | Counters |
| `radio CH MBPS` | Channel and rate until reset (default 11, 1 Mbps) |
| `keys` | Pair key ID and current session |
| `reboot` | Restart (new session key) |

## Limits

- The sketch-based mic and speaker have not run on hardware yet. The ADC
  DMA ran at 48 kHz before; the boot `MIC` lines show whether it delivers
  64 kHz. The speaker's timer interrupt is new; `speaker_rate_hz` in
  `report` shows its real rate.
- The sketch's keys are typed as lines here: `+` then Enter, and so on.
- Howl detection is on the speaker board, where the sound comes out. A
  sustained loud tone or a long shout can be notched like real feedback,
  as in the sketch; `r` clears the notches.
- Half duplex: a board drops incoming voice while it is talking.
- E2 is a listening test; E1 gives the numbers.
