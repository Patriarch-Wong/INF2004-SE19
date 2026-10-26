# ESP-NOW relay without the end-to-end key

Stage 3 of the end-to-end security tests in
[e2e_security_test](../e2e_security_test/README.md). Board 3 forwards
protected messages between handsets 1 and 2. It holds the network key only,
so it can check the hop tag and read the routing header but cannot decrypt
or undetectably modify messages. The handsets time a ping/pong round trip
with and without the relay to measure the latency it adds.

Status: hardware-complete. Host tests pass, all environments build, and
all five hardware tests passed on 2026-10-27. Relaying adds about 3.2 ms
per message (median round trip 13.6 ms via the relay, 7.2 ms direct).

## What is reused

The frame format, crypto, replay window and receive order are unchanged
from stages 1–2; see [Protected message
format](../e2e_security_test/README.md#protected-message-format). These
files are copied byte for byte: `include/secure_message.h`,
`src/secure_message.cpp`, `include/crypto_backend.h`,
`src/crypto_backend_mbedtls.cpp`, `include/replay_window.h` and `tests/`.
The relay calls `secure_message::verify_hop()`, which authenticates the
header and ciphertext with the network key only.

## Keys

| Key | Held by | Purpose |
| --- | --- | --- |
| Pair 1–2 | Handsets 1 and 2 | AES-128-GCM encryption and end-to-end tag |
| Network | Handsets and the relay | Hop tag the relay checks |

Generate the gitignored `include/e2e_keys.h` once. These keys are separate
from `../e2e_security_test`'s; reflashing that project restores its own.

```sh
python3 tools/generate_keys.py            # handsets 1-2
python3 tools/generate_keys.py --devices 3
```

The network key is outside the device blocks. Under `E2E_RELAY` the file
defines no pair keys, and `include/key_store.h` defines neither
`peer_key` nor `pair_key()`, so relay code that tries to use a pair key
does not compile. The relay build also compiles only `relay.cpp`,
`secure_message.cpp` and the crypto backend.

`tools/find_keys.py` reports which keys from `e2e_keys.h` appear in any
file, as raw bytes or hex text. Use it on firmware images (test 4) and on
evidence logs before committing them:

```sh
python3 tools/find_keys.py .pio/build/*/firmware.bin evidence/stage3/*.txt
```

## Devices

`include/network_config.h`: devices 1 and 2 are handsets; device 3
(`E0:72:A1:D7:F6:50`) is the relay. `ping_source_id` (1) is the handset
that sends pings.

| Board | MAC | Port (CH343 serial number) |
| --- | --- | --- |
| 1 | `E0:72:A1:D5:64:0C` | `/dev/cu.usbmodem5B420192981` |
| 2 | `E0:72:A1:D7:FA:B8` | `/dev/cu.usbmodem5B5F0215971` |
| 3 | `E0:72:A1:D7:F6:50` | `/dev/cu.usbmodem5B420192791` |

Confirm ports with `pio device list`. Every board stops at boot if its MAC
does not match the environment it was built for.

## Build environments

| Environment | Board | Purpose |
| --- | --- | --- |
| `board1`, `board2` | 1, 2 | Handset; every frame goes to the relay's MAC |
| `board1-direct`, `board2-direct` | 1, 2 | Handset sending straight to the peer; latency baseline only |
| `board3-relay` | 3 | Relay: network key only |
| `board3-relay-tamper` | 3 | Relay that flips a ciphertext bit and re-signs the hop |

`pio run` builds `board1`, `board2` and `board3-relay`. All use the UART
connector.

## Handset behaviour

Board 1 sends `PING SE19 #n` once a second. Board 2 sends nothing on its
own: it answers each accepted ping with `PONG SE19 #n` under its own
counter. The payload starts with a kind byte and the sequence number, then
the text. The receive path is the same as in stages 1–2: hop tag, pair key
selected by the authenticated source, end-to-end tag, destination check,
replay window. Received frames arrive from the relay's MAC, so `RX` lines
show `E0:72:A1:D7:F6:50 | from <peer>`; the pair key, not the MAC,
identifies the sender.

Board 2 sends the pong before printing, so its `TX ... PONG n` line comes
before its `RX ... "PING SE19 #n"` line. Serial output is buffered (4 KB)
on every board so printing does not stall the radio path being timed.

The NVS transmit counter (`e2e/limit`) carries over from earlier stages and
only moves forward, which is safe with new keys.

## Relay behaviour

At boot the relay runs the crypto self-test, refuses an all-zero network
key, stops unless its MAC is the `role::relay` entry, and prints `Pair
keys held: 0`. For each frame:

1. Drop frames from MACs not in `network_config.h` (counted as unknown-MAC
   drops), with the same callback filters and queue as the handset.
2. `verify_hop()`. On failure print `DROP <mac> | <n> bytes | <status>`.
3. Drop if the destination ID is unknown, is the relay, or is the sending
   device.
4. Tamper build only: flip bit 0 of the first ciphertext byte, then
   `sign_hop()`.
5. Forward the bytes and print `FWD <src> -> <dst> | counter <n> | <bytes>
   bytes | <result> | <hex>`.

There is no replay check at the relay; the destination does that end to
end. Every 10 s: `Status | forwarded | dropped | send failures | queue
drops | unknown MAC drops | relay us min/median/max (n)`.

## Latency

Clocks are not synchronised, so two things are measured.

- **Relay processing** (relay status line and each `FWD` line): from the
  receive callback to the send-done callback of the forwarded frame, with
  `esp_timer_get_time()`. It covers queueing, the hop check, and
  transmitting the forwarded frame until its MAC-layer ACK. The relay
  forwards one frame at a time and waits for its send-done callback before
  the next, so each callback matches its frame. Only delivered frames count;
  frames without an ACK count as send failures.
- **Round trip** (board 1): from just before sending ping *n* to accepting
  pong *n*, printed as `RTT n | <us>` with min/median/max over the last 256
  samples in the status line. Run once through the relay and once with the
  `-direct` builds and the relay off. Added one-way cost per relayed
  message = (median relayed RTT − median direct RTT) / 2.

## Host tests

```sh
tests/run_host_tests.sh
```

Requires `brew install openssl@3`, or set `OPENSSL_PREFIX`. These are the
stage 1–2 tests, unchanged; they include the relay case (verify with the
network key only, then the forwarded bytes still open).

## Hardware test procedure

Run from this directory. For every upload: hold BOOT, tap RESET, release
BOOT, upload, then press RESET (the RTS auto-reset does not work). Close
serial monitors before uploading.

```sh
pio run -e board3-relay -t upload --upload-port /dev/cu.usbmodem5B420192791
pio run -e board1 -t upload --upload-port /dev/cu.usbmodem5B420192981
pio run -e board2 -t upload --upload-port /dev/cu.usbmodem5B5F0215971
```

Logs are captured with `tools/capture_logs.py` (run it with
`~/.platformio/penv/bin/python`, which has pyserial). Opening a port
resets that board, so the second port opens after `--delay` seconds and
the first board is already running when the second boots. Power the board
that is not captured separately and leave it running. Each run writes
`evidence/stage3/<run>_<label>.txt`.

```sh
PY=~/.platformio/penv/bin/python
B1=/dev/cu.usbmodem5B420192981
B2=/dev/cu.usbmodem5B5F0215971
B3=/dev/cu.usbmodem5B420192791
```

1. **Forwarding.** Board 2 powered separately:
   `$PY tools/capture_logs.py forward-1 board3=$B3 board1=$B1 --delay 12
   --duration 60`. Then board 1 powered separately:
   `$PY tools/capture_logs.py forward-2 board3=$B3 board2=$B2 --delay 12
   --duration 60`. Pass: each handset prints `RX E0:72:A1:D7:F6:50 | from
   <peer>` for every counter and 0 rejected; the relay prints a matching
   `FWD` line for each; `50494E47` (`PING`) and `504F4E47` (`PONG`) never
   appear in relay output.
2. **No direct path.** Board 3 powered separately and running `board3-relay`:
   `$PY tools/capture_logs.py relay-off board1=$B1 board2=$B2 --delay 3
   --duration 45`. At about 15 s hold board 3's RESET for ~10 s, then
   release. Pass: RX stops on both handsets while held and resumes after.
3. **Tampering relay.** Flash board 3 with `board3-relay-tamper`, board 1
   powered separately: `$PY tools/capture_logs.py tamper board3=$B3
   board2=$B2 --delay 12 --duration 40`. Pass: board 2 prints `REJECT
   E0:72:A1:D7:F6:50 | ... | bad end-to-end tag` for every relayed ping and
   accepts 0. Board 2 then sends no pongs, so board 1 has nothing to reject
   and must show `accepted 0` and no `RTT` lines.
4. **Keys in binaries.** `python3 tools/find_keys.py
   .pio/build/*/firmware.bin`. Pass: relay images hold the network key
   only; handset images hold the network and 1–2 keys.
5. **Latency.** The relayed figures come from test 1's `forward-1` logs
   (board 1 `RTT`, board 3 `relay us`). For the baseline flash
   `board1-direct` and `board2-direct`, unplug board 3, then
   `$PY tools/capture_logs.py direct board2=$B2 board1=$B1 --delay 5
   --duration 60`. Pass: ≥30 RTT samples in each configuration, relay
   processing min/median/max, and the derived per-hop cost.

Every test: no queue drops, no `WARNING`, and receivers not reset mid-run
except where the test says so. Run `tools/find_keys.py` on the logs before
committing.

## Results

Test 1, 2026-10-27, all three boards on USB (boards 2 and 3 through a hub).
Two 60 s runs, each opening board 3 at 0 s and a handset at 12 s while the
other handset kept running: `forward-1` (board 3 + board 1) and
`forward-2` (board 3 + board 2). Logs are in `evidence/stage3/`.

- Board 1 accepted 48 pongs, counters 24832–24879 with no gaps, every one
  as `RX E0:72:A1:D7:F6:50 | from 2`. Board 2 accepted 48 pings, counters
  23613–23660 with no gaps, every one as `RX E0:72:A1:D7:F6:50 | from 1`.
  Last status lines: `accepted 39 | rejected 0 | queue drops 0 | unknown
  MAC drops 0` and `accepted 40 | rejected 0 | ...` (printed at 52 s).
- The relay printed a `FWD` line for every counter each handset sent and
  every counter it received: 120 per run, all `delivered`. Final status
  lines: `forwarded 118 | dropped 0 | send failures 0 | queue drops 0 |
  unknown MAC drops 0` and `forwarded 120 | dropped 0 | ...`.
- `50494E47` (`PING`), `504F4E47` (`PONG`) and `48454C4C4F` (`HELLO`)
  never appear in relay output. No `REJECT`, `DROP`, `WARNING` or
  mid-run resets on any board. `tools/find_keys.py` found no keys in the
  four logs.

Relayed latency, from the same runs (test 5, relayed half):

| Measure | n | Min | Median | Max |
| --- | --- | --- | --- | --- |
| Relay processing, `forward-1` | 120 | 2113 us | 2450 us | 10504 us |
| Relay processing, `forward-2` | 120 | 2117 us | 2519 us | 15828 us |
| Round trip via relay, board 1 | 47 | 9511 us | 13559 us | 21219 us |

Relay processing above ~4 ms is spread through both runs and every such
frame was still delivered, consistent with MAC-layer retries on channel 1.
Board 1 sent 48 pings; pong 48 arrived at 60.03 s, as the capture ended,
before its `RTT` line printed.

Test 2, 2026-10-27: board 3 running `board3-relay` on USB power with its
port closed; boards 1 and 2 captured for 75 s (`relay-off`), board 2
opened at 3 s. Board 3's RESET was held from about 23 s to 41 s.

- Board 1 kept sending (`TX ... PING 24` to `PING 40`, 18 frames) but
  received nothing from 23.04 s to 41.12 s; the first frame after release
  was `RX E0:72:A1:D7:F6:50 | from 2 | ... "PONG SE19 #41"`.
- Board 2 received nothing in the same window: its RX counters jump from
  24599 to 24617, so the 17 pings sent while the relay was held never
  arrived directly, and it sent no pongs.
- Both resumed within one ping of release with `rejected 0`, no queue
  drops, no `WARNING` and no resets besides the capture's own (board 2 at
  3 s). Board 1's earlier counter jump (25921 to 26625) is board 2's
  reboot moving to its next 1024-counter block.

Test 3, 2026-10-27: board 3 flashed with `board3-relay-tamper` (boot line
`Build mode: relay, TAMPER (negative test)`, `Pair keys held: 0`). Two
40 s runs, each opening board 3 at 0 s and a handset at 12 s: `tamper`
(board 3 + board 2) and `tamper-1` (board 3 + board 1).

- Board 2 printed `REJECT E0:72:A1:D7:F6:50 | ... | bad end-to-end tag`
  for all 28 pings the relay forwarded after it booted, and accepted none
  (`accepted 0 | rejected 20 | queue drops 0` at 32 s). The hop tag was
  valid, so only the end-to-end tag caught the change.
- Comparing board 1's `TX` hex with the relay's `FWD` hex for the same 28
  counters: each forwarded frame differs only in bit 0 of byte 8 (the
  first ciphertext byte) and in the re-signed hop tag.
- Board 2 sent no pongs, so board 1 printed no `RX` or `RTT` lines and
  showed `accepted 0 | rejected 0 | queue drops 0`. The relay reported
  `dropped 0 | send failures 0` in both runs. No `WARNING` on any board.

Test 5, 2026-10-27, direct baseline: boards 1 and 2 flashed with
`board1-direct` and `board2-direct`, captured for 60 s (`direct`, board 2
at 0 s, board 1 at 5 s). Board 3 stayed powered (still `board3-relay-tamper`)
and was logged throughout: `forwarded 0 | dropped 0`, so it was not in the
path. Board 1 received every pong from board 2's own MAC (`RX
E0:72:A1:D7:FA:B8 | from 2`); 55 pings, 55 RTTs, `rejected 0` and no queue
drops on either handset.

| Round trip, board 1 | n | Min | Median | 75th percentile | Max |
| --- | --- | --- | --- | --- | --- |
| Direct (`direct`) | 55 | 5515 us | 7193 us | 10198 us | 19222 us |
| Via relay (`forward-1`) | 47 | 9511 us | 13559 us | 16728 us | 21219 us |

Added one-way cost per relayed message: (13559 − 7193) / 2 ≈ **3.2 ms**.
The relay's own processing median was 2.45–2.52 ms (receive callback to
the forwarded frame's ACK); the rest is the extra radio reception at the
relay. Test 2's `relay-off` run, outside the hold, gave a similar relayed
median of 14211 us (n 57).

Test 4, 2026-10-27, with keys generated that day: `tools/find_keys.py`
reported `network` for `board3-relay` and `board3-relay-tamper`, and
`network, pair 1-2` for `board1`, `board2`, `board1-direct` and
`board2-direct`. As a control, searching `board1`'s image for the keys of
`../e2e_security_test` reported none. A file that calls
`key_store::pair_key()` fails to compile with `-DE2E_RELAY`.

| # | Test | Result |
| --- | --- | --- |
| 1 | Forwarding through the relay | Pass |
| 2 | No direct path | Pass |
| 3 | Tampering relay | Pass |
| 4 | No pair key in the relay binary | Pass |
| 5 | Latency | Pass: ≥47 samples each; +3.2 ms per relayed message |

## Limits

- **Single hop:** the relay forwards from one handset to another; there is
  no multi-hop routing.
- **No relay-side replay filtering:** a replayed frame is forwarded and
  rejected at the destination.
- **Network key:** the relay can produce valid hop tags, so the tamper test
  shows the end-to-end tag, not the hop tag, is what protects content.
- **Not covered:** LoRa, voice, pairing and session keys (stage 4),
  receiver-reboot replay.

## Project files

- `src/main.cpp`: handset firmware: next-hop routing, ping/pong, NVS
  counter, receive order, status line.
- `src/relay.cpp`: relay firmware and tamper build.
- `include/latency_stats.h`: min/median/max over recent samples.
- `include/key_store.h`, `include/e2e_keys.example.h`,
  `tools/generate_keys.py`: key layout with a relay block that holds no
  pair keys.
- `include/network_config.h`, `include/app_config.h`: devices, roles,
  ping source and settings.
- `tools/capture_logs.py`: two-port capture with shared timestamps.
- `tools/find_keys.py`: key search for firmware images and logs.
- Copied from stage 1–2: `secure_message`, `crypto_backend`,
  `replay_window` and `tests/`.
