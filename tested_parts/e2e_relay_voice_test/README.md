# Two-way Opus voice through a relay

Handsets 1 and 2 both send and receive Opus voice over the end-to-end
encrypted link. Board 3 sits in the middle as a relay that holds only the
network key. Every frame, in both directions, goes handset → relay →
handset. [SPEC.md](SPEC.md) defines the run and its checks.

Status: **hardware run 3 passes all six checks**, including simultaneous
voice in both directions through the relay, on channel 11 at 6 Mbps
(2026-10-29). The rate is set at runtime and resets on reboot.

## Design and reuse

Built from Stage 6 (`e2e_opus_test`) with Stage 3's relay idea
(`e2e_relay_test`). Unchanged from Stage 6: `secure_message`,
`replay_window`, `crypto_backend`, `kdf`, `control_frame`, `pairing`,
`session`, `voice`, `codec`, `opus_player`, their host tests,
`capture_logs.py` and `partitions.csv`. NVS keeps the Stage 4 format, so
boards 1 and 2 keep their pair key (`48FF15EF`) and need no re-pairing.

What changed:

- **Payload** (`opus_voice`): a 4-byte random stream ID after the marker,
  so a handset can tell one incoming stream from the next. The payload is
  90 bytes and the frame 130 bytes on the air.
- **Handset** (`main.cpp`, `opus_app.inc`): either handset can send
  (`voice` streams to the other handset), and both always listen. One codec
  task makes every Opus call, as the library is not thread safe. It encodes
  this handset's frames while it waits for the next playout slot, and
  between frames of the in-order replay, so a handset can send and receive
  at once. A receiver locks onto one stream at a time (`stream_lock.h`,
  host-tested). Frames from another stream count as foreign, and late
  frames of the finished stream count as stale.
- **Routing**: data and session frames go to the relay's MAC. A handset
  rejects any frame that is not pairing and did not come from the relay
  (`REJECT ... not via the relay`). Pairing is still broadcast between
  handsets side by side, and the relay ignores it.
- **Relay** (`relay.cpp`, `board3-relay`): checks the hop tag, checks that
  the header's source is the sending MAC's device and that the destination
  is the other handset, then forwards the bytes unchanged. It queues multiple
  frames in flight and holds a frame for retry when ESP-NOW has no room. It logs `FWD type
  src dst counter bytes result relay_us` per frame. Its build has no pair
  key, NVS, session, pairing or Opus code.
- **Radio**: receive queue 32 deep (the relay carries both directions), and
  each queued frame keeps its receive-callback time.
- **Text**: each handset sends `HELLO SE19 #n` to the other once a second,
  so both directions of every session carry traffic even without voice.
- The live buffer is 200 ms. Delay is acceptable here; delivery is what
  matters. Late and lost frames are also counted at 60–300 ms from the
  stored arrivals.

### Keys

`include/e2e_keys.h` is **committed on purpose**. This is a school project
and the key protects nothing of value. It is a fresh network key, separate
from Stage 6's. The network key only signs the hop: a holder can make
frames the relay forwards, but cannot open or forge an end-to-end frame. It
could, however, try to man-in-the-middle a new pairing, which the six-digit
code comparison is there to catch. End-to-end confidentiality rests on the
pair keys, which pairing creates and which never leave each handset's NVS.
`.gitignore` keeps NVS or flash read-backs, NVS CSVs and core dumps out of
the repo.

All three boards must be flashed from this project, because their network
keys must match.

## Builds and host checks

From this directory:

```sh
pio run
sh tests/run_host_tests.sh
sh tests/run_opus_tests.sh
```

| Environment | Board | Function |
| --- | --- | --- |
| `board1` | 1 | Handset: sends and receives, via the relay |
| `board2` | 2 | Handset: sends and receives, via the relay |
| `board3-relay` | 3 | Relay, network key only |

Validated 2026-10-29: the three environments build with no warnings. The
secure-message, pairing, session and Opus suites pass. So do the voice and
stream-lock tests (also run under ASan/UBSan) and the 10 analysis tests.

## Manual hardware procedure

The user flashes every board. **Do not automate uploads.** BOOT + RESET as
needed, then from this directory:

```sh
pio run -e board1 -t upload --upload-port /dev/cu.usbmodem5B420192981
pio run -e board2 -t upload --upload-port /dev/cu.usbmodem5B5F0215971
pio run -e board3-relay -t upload --upload-port /dev/cu.usbmodem5B420192791
```

After uploading, use the successful 6 Mbps capture procedure below.

## Results

Earlier baseline (1 Mbps), `evidence/relay/run2_analysis.json`:

- R1 relay path, R2 sessions, R3 one-way voice, and R5 handset pipelines pass.
- Each one-way stream delivers 3,000/3,000 packets with matching CRCs and
  no late/lost frames at 200 ms; in-order correlation 0.9989, SNR 26.73 dB.
- R4 simultaneous voice fails: 1 → 2 receives 1,577/3,000 packets;
  2 → 1 receives 1,825/3,000. R6 fails with 2,491 relay queue drops.
- `probe_radio_*` captures confirmed that the previously connected firmware did not
  implement the current `radio CH MBPS` command. The handsets print their
  old help, and the relay prints status without a `RADIO ... ok` response.
- Current builds and all host suites pass. These software checks do not
  establish that the hardware bottleneck is fixed.

### Successful hardware run (2026-10-29)

`evidence/relay/run3_rate6_analysis.json` reports all checks R1–R6 passing.
Both one-way streams and both simultaneous streams each delivered
3,000/3,000 voice packets (12,000 total), with all CRCs matching. Every
stream had zero late/lost frames at 200 ms and `LIVE_MISSED 0`; live and
in-order audio both measured correlation 0.9989 and SNR 26.73 dB.

Relay queue drops, explicit drops, send errors, and handset pipeline drops
were all zero. Relay forwarding latency was median 4.696 ms, p99 61.196 ms.
Capture-to-send p99 was 12.106 ms on board 1 and 12.837 ms on board 2.
All 14,932 accepted data frames had corresponding relay forwarding records.

There were two unacknowledged text forwards during staggered startup,
before rate selection and voice testing, and ten replay rejections on board
1. A subsequent [trace audit](evidence/relay/run3_replay_investigation.md)
accounts for all ten: each affected counter was sent once by board 2,
forwarded twice by the relay, and accepted once by board 1. Replaying the
forwarded counters through the actual replay window reproduces all ten
rejections. The origin of duplicate radio delivery remains unconfirmed;
the rejections did not cause missing voice packets. This is one bench run,
not a range or long-duration reliability test.

### Reproduce the successful run

Keep channel 11 and change only the transmit rate to 6 Mbps on all three
boards, using `radio 11 6`. Require `RADIO channel 11 rate 6 ok` from each
board before interpreting the test. Radio settings reset on reboot/open.
Run the same full test with a new evidence name and the three additional
commands before the first voice stream:

```sh
caffeinate -dims ~/.platformio/penv/bin/python tools/capture_relay.py run3_rate6 \
  relay=/dev/cu.usbmodem5B420192791 \
  board2=/dev/cu.usbmodem5B5F0215971@2 \
  board1=/dev/cu.usbmodem5B420192981@4 \
  --baud 921600 --duration 270 \
  --send 'relay@8=radio 11 6' \
  --send 'board1@8=radio 11 6' --send 'board2@8=radio 11 6' \
  --send board1@12=voice --send board2@95=voice \
  --send board1@178=voice --send board2@178.5=voice \
  --send board1@262=report --send board2@262=report --send relay@262=status

python3 tools/analyse_run.py --board1 evidence/relay/run3_rate6_board1.txt \
  --board2 evidence/relay/run3_rate6_board2.txt \
  --relay evidence/relay/run3_rate6_relay.txt > evidence/relay/run3_rate6_analysis.json
```

This run supports using 6 Mbps for this bench setup. Firmware still boots
at 1 Mbps; the commands above apply 6 Mbps after each reset. Use a fresh run
name when repeating so the saved evidence is not overwritten.

## Files

- `src/main.cpp`, `src/opus_app.inc`: handset, with Stage 4 pairing,
  relay routing and two-way voice.
- `src/relay.cpp`: relay firmware.
- `include/stream_lock.h`: which incoming stream a handset plays.
- `include/opus_voice.h`, `src/opus_voice.cpp`: payload with stream ID,
  test signal, CRC32, playout rule, audio comparison.
- `tools/analyse_run.py`: checks R1–R6 from the three logs.
- `tools/capture_relay.py`: capture tool writing to `evidence/relay/`.
- `tests/`: inherited suites, Opus, voice and stream-lock tests, analysis
  tests.
