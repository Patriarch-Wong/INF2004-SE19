# Encrypted one-to-many Opus voice, without a relay

Board 1 broadcasts each encrypted packet once to FF:FF:FF:FF:FF:FF.
Boards 2 and 3 independently decrypt and play the same generated audio.
There is no relay and no per-receiver unicast copy. One identical firmware
image goes on all three boards; their MAC addresses select board numbers.

Status: firmware builds and host tests pass. Hardware run 1 **fails** at
6 Mbps (2026-10-29); run 2 at 1 Mbps **passes** every check (2026-10-30).
Use the `encrypted-broadcast-1m` build for broadcast.

## Test

Run two 60-second streams from board 1, with 3,000 frames each:

- `voice plain`: current audio packet only. **Still encrypted**; "plain"
  is the inherited command name for no redundancy.
- `voice red`: current and previous audio packets, both inside encryption.

Channel 11, 6 Mbps from startup, power save off. Opus: 16 kHz mono,
24 kbit/s CBR, 20 ms frames, complexity 5. Playback buffer: 200 ms.
This uses generated test audio, not microphones or speakers.
All boards can send, but this first run uses only board 1 as sender.

## Keys and protection

`include/group_keys.h` contains two generated, local 128-bit group keys:
an audio key for AES-128-GCM and a separate HMAC hop key. Both are shared
by all three boards. The file is ignored by git; it is already generated
in this working copy. In a fresh checkout, run:

```sh
python3 tools/generate_group_keys.py
```

The generator refuses to overwrite an existing file. This test does not
use or modify the existing paired handset keys. Reused `secure_message`
seals the entire broadcast payload, including its marker and Opus bytes.
The authenticated outer header carries source, group destination 255,
type and counter. The old hop tag is retained for code reuse, although
there is no relay. Actual packets are 133 bytes without redundancy and
195 with redundancy (the maximum supported frame is 235 bytes).
ESP-NOW's link encryption is off; protection is in the application payload.
Broadcast radio callbacks do not prove delivery; both receiver logs do.

Each sender reserves 65,536 counters ahead in its separate `groupcast-v1`
NVS namespace before using any. Reboot skips the remainder of that block.
Flash-write failures and counter exhaustion stop transmission. Do not erase
or restore this NVS state while retaining the same keys: generate fresh
keys and flash all three boards if the counter state is lost. A normal
firmware upload preserves NVS. Keys remain in the firmware image and are
not protected against physical extraction.

Authenticated source IDs must match the sending MAC's allowlisted board.
Each receiver has a 64-counter replay window per sender, updated only after
authentication. Replay windows reset when the receiver reboots; rejection
of recorded traffic across a receiver reboot is **not** provided. Group
members can decrypt and forge group traffic: this authenticates membership,
not an individual sender. There is no pairing, key distribution, rekeying,
forward secrecy or sender signature in this experiment.

## Build and validation

```sh
pio run
sh tests/run_tests.sh
```

Tests cover Opus, frame parsing and redundancy, receiver playback, the
inherited analysis, and the new one-to-many analysis. Group crypto tests
exercise two independent receivers decrypting the same ciphertext, wrong
audio/hop keys, every single-byte mutation, every truncation, MAC/source
binding, zero counter rejection, and replay rejection under ASan/UBSan.
These are host checks, not an over-the-air confidentiality measurement.
With all three boards occupied, there is no separate sniffer in this run.

## Manual upload

The user flashes every board. **Do not automate uploads.** From this folder,
use BOOT + RESET as needed:

```sh
pio run -e encrypted-broadcast -t upload --upload-port /dev/cu.usbmodem5B420192981
pio run -e encrypted-broadcast -t upload --upload-port /dev/cu.usbmodem5B5F0215971
pio run -e encrypted-broadcast -t upload --upload-port /dev/cu.usbmodem5B420192791
```

## Capture and pass criteria

Open the receivers first, then the sender. Each receiver takes about 15 s
after a stream to finish its in-order audio comparison. No re-pairing or
runtime radio-rate commands are required. Choose a new name on repeat runs.

```sh
caffeinate -dims ~/.platformio/penv/bin/python tools/capture_broadcast.py run1 \
  board3=/dev/cu.usbmodem5B420192791 \
  board2=/dev/cu.usbmodem5B5F0215971@2 \
  board1=/dev/cu.usbmodem5B420192981@4 \
  --baud 921600 --duration 200 \
  --send 'board1@12=voice plain' --send 'board1@105=voice red'
python3 tools/analyse_group.py evidence/encrypted_broadcast/run1 \
  > evidence/encrypted_broadcast/run1_analysis.json
```

`analyse_group.py` exits nonzero on failure. It requires encrypted firmware
and 6 Mbps boot messages from all three boards, two streams from board 1,
exactly receivers 2 and 3 for each, authenticated receptions and no security
rejections. Every stream must send 3,000 packets without send/callback or
pipeline errors. Each receiver must get at least 99% of primary packets,
have zero CRC mismatches, under 1% late/missing at 200 ms, no missed live
slots, and in-order correlation >0.99 / SNR >20 dB. Missing reports fail.
Per-receiver loss, redundant recovery and shared-loss patterns are included.
Timing logged by the inherited OPUS_TX line stops before encryption; no
mouth-to-ear delay estimate is claimed for this new encrypted transport.

## Reuse

Based on `opus_broadcast_test`, with its codec, player, redundancy and
reporting retained. `group_crypto` and the radio wrapper add encryption,
MAC/source validation, replay checks and NVS counter reservation using the
previous E2E test's `secure_message`, crypto backend and replay window.
`analyse_broadcast.py` is retained as a parser; use `analyse_group.py` for
this experiment's verdict, since the original requires all boards to send.

## Hardware run 1: 2026-10-29

Evidence: `evidence/encrypted_broadcast/run1_*`; verdict in
`run1_analysis.json`. User confirmed all three boards were close together.
All booted into encrypted group broadcast on channel 11 at 6 Mbps.
Board 1 sent both 3,000-frame streams with no send errors, failed callbacks,
or pipeline drops. Both receivers authenticated packets; security rejects,
CRC mismatches, receive queue drops and codec drops were zero.

Without redundancy, board 2 received 2,705/3,000 (295 lost, 9.83%);
board 3 received 2,204/3,000 (796 lost, 26.53%). Audio SNR was 9.11 dB
and 4.92 dB respectively. 112 packets were lost at both receivers;
867 were lost at just one receiver.

With redundancy, board 2 received 2,761 primary packets and recovered 217,
leaving 22 missing (0.73%), SNR 19.41 dB. Board 3 received 2,231 primary
packets and recovered 556, leaving 213 missing (7.10%), SNR 10.56 dB.
104 primary packets were lost at both receivers; 800 at just one receiver.
The redundancy helped, but neither reception met the complete criteria.

The analyser passes setup, one-to-many topology and authenticated reception,
but fails the stream criteria. This demonstrates encrypted fan-out, not
reliable voice delivery. Successful broadcast send callbacks do not imply
that either receiver got the frame. The loss cause remains unconfirmed;
zero queue/security drops point away from those counted failure paths.

Next comparison: keep keys, channel, board placement and audio settings
fixed, and compare 1 Mbps transmission against this 6 Mbps baseline. There is no sniffer or wrong-key hardware run here;
negative cryptographic checks remain host tests.

## Run 2: same test at 1 Mbps (T1)

Only the PHY rate changes. `encrypted-broadcast-1m` is the same firmware
with `RADIO_RATE_MBPS=1`, so every board boots with
`RADIO channel 11 rate 1 ok`. Keys, NVS counters, channel 11, the 200 ms
buffer, audio settings and board placement stay as in run 1 (boards close
together). Flash all three boards:

```sh
pio run -e encrypted-broadcast-1m -t upload --upload-port /dev/cu.usbmodem5B420192981
pio run -e encrypted-broadcast-1m -t upload --upload-port /dev/cu.usbmodem5B5F0215971
pio run -e encrypted-broadcast-1m -t upload --upload-port /dev/cu.usbmodem5B420192791
```

Capture and analyse with `--rate 1`, which fails setup unless every board
reports 1 Mbps:

```sh
caffeinate -dims ~/.platformio/penv/bin/python tools/capture_broadcast.py run2_rate1 \
  board3=/dev/cu.usbmodem5B420192791 \
  board2=/dev/cu.usbmodem5B5F0215971@2 \
  board1=/dev/cu.usbmodem5B420192981@4 \
  --baud 921600 --duration 200 \
  --send 'board1@12=voice plain' --send 'board1@105=voice red'
python3 tools/analyse_group.py evidence/encrypted_broadcast/run2_rate1 --rate 1 \
  > evidence/encrypted_broadcast/run2_rate1_analysis.json
```

### Run 2 result: 2026-10-30

Evidence: `evidence/encrypted_broadcast/run2_rate1_*`; verdict in
`run2_rate1_analysis.json`, which **passes** every check. All three boards
booted with `RADIO channel 11 rate 1 ok`, boards close together as in
run 1. Board 1 sent both 3,000-frame streams with no send errors or failed
callbacks. Security rejects, CRC mismatches and pipeline drops were zero.

| Stream (ch 11, 200 ms buffer) | Rate | Board 2 primary | Board 2 SNR | Board 3 primary | Board 3 SNR |
| --- | --- | --- | --- | --- | --- |
| `voice plain` (no redundancy) | 6 Mbps (run 1) | 2,705 (9.83% lost) | 9.11 dB | 2,204 (26.53% lost) | 4.92 dB |
| `voice plain` (no redundancy) | 1 Mbps (run 2) | 2,997 (0.10% lost) | 25.61 dB | 3,000 | 26.73 dB |
| `voice red` | 6 Mbps (run 1) | 2,761, 0.73% missing | 19.41 dB | 2,231, 7.10% missing | 10.56 dB |
| `voice red` | 1 Mbps (run 2) | 3,000 | 26.73 dB | 3,000 | 26.73 dB |

Board 2's 3 lost frames in the plain stream were concealed; correlation
0.9986. Only the rate changed between runs, so the run 1 loss came from
the 6 Mbps rate, not from encryption: broadcast frames get no ACK or
retry, and the 6 Mbps OFDM rate is less robust than 1 Mbps DSSS. This
matches `opus_broadcast_test` (unencrypted, 1 Mbps, 2/36,000 lost).
