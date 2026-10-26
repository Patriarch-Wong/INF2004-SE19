# ESP-NOW end-to-end security test

Test bench for the SE19 change requirement: end-to-end confidentiality and
authenticity for voice and text, a relay that forwards without decrypting,
and rejection of modified, forged and replayed packets.

[espnow_encrypt_test](../espnow_encrypt_test/) used ESP-NOW's built-in CCMP.
That encrypts each radio hop, and the receiving Wi-Fi driver decrypts it, so a
relay would see plaintext, LoRa would be unprotected, and replays would pass.
This project seals each message once in application code, at the source
handset, and opens it only at the destination handset. ESP-NOW link
encryption is off here, so every test exercises only the application layer.

Status: stages 1 and 2 hardware-complete. All six stage 1 steps and all
three stage 2 attacker modes passed on 2026-10-27.

| Stage | Boards | Covers | Status |
| --- | --- | --- | --- |
| 1 | 2–3 | Message format, pairwise keys, replay window, tamper/forge/replay/impersonate builds | Hardware-tested; all steps pass |
| 2 | 3 | Board 3 as attacker: over-the-air replay, spoofed MAC, unknown device | Hardware-tested; all modes pass |
| 3 | 3 | Board 3 as relay without the end-to-end key; per-hop latency | Hardware-tested in [e2e_relay_test](../e2e_relay_test/); all tests pass |
| 4 | 3 | Pairing (BOOT button or serial), keys in NVS, session keys | Planned |
| 5 | 2–3 | Confidentiality capture, synthetic voice latency | Planned |

## Protected message format

One frame fits in one ESP-NOW packet (250 bytes in IDF 4.4) and is transport
neutral, so LoRa text can carry the same bytes.

| Offset | Size | Field | Protection |
| --- | --- | --- | --- |
| 0 | 1 | Version (1) | Authenticated |
| 1 | 1 | Type: 1 text, 2 voice | Authenticated |
| 2 | 1 | Source device ID | Authenticated |
| 3 | 1 | Destination device ID | Authenticated |
| 4 | 4 | Counter, little-endian | Authenticated |
| 8 | n | Payload ciphertext | Encrypted and authenticated |
| 8 + n | 16 | End-to-end tag (AES-128-GCM) | — |
| 24 + n | 16 | Hop tag (HMAC-SHA256, truncated) | — |

Overhead is 40 bytes, leaving up to 210 bytes of payload.

- **End-to-end layer:** AES-128-GCM under the pair key shared by the source
  and destination handsets, with the 8-byte header as associated data. The
  header can be read but not changed. The 12-byte nonce is source ID,
  destination ID, six zero bytes, and counter.
- **Hop layer:** HMAC-SHA256 under the network key over everything before the
  hop tag. A relay holds only the network key. It can authenticate the header
  and ciphertext and read the routing fields, but it cannot decrypt.
  A network-key holder can re-sign a modified frame, but the end-to-end
  tag still rejects it.
- **Receive order:** hop tag and header check, then the hop-authenticated
  source ID selects the pair key, then the end-to-end tag, destination check
  and replay window. Only authenticated counters reach the window, so forged
  frames cannot advance it.

## Counters and replay protection

- **Transmit counters** start at 1 and are never reused per source and key,
  because a repeated GCM nonce breaks confidentiality. They survive reboots:
  the firmware reserves blocks of 1024 in NVS (`e2e/limit`). After a reset,
  the counter jumps to the next block. At 50 voice frames per second, that
  is one NVS write every ~20 s.
- **Replay window** (`include/replay_window.h`): one per source, 64 counters
  wide. A counter is accepted once. Counters up to 63 behind the highest are
  allowed for reordering, and older ones are rejected.

## Keys

| Key | Held by | Purpose |
| --- | --- | --- |
| Pair key, one per pair of handsets | The two handsets of that pair | AES-128-GCM encryption and end-to-end tag |
| Network | Handsets and relays | Hop tag the relay checks |

**Pairwise keys identify the sender.** With one key shared by every
handset, a valid tag only proves the frame came from some key holder, so
handset 3 could claim to be handset 1. With pairwise keys, a frame that
opens under the 1–2 key can only have come from device 1 or 2, and device 2
knows it did not send it. Three handsets need three keys (1–2, 1–3, 2–3);
*n* handsets need *n*(*n*−1)/2. A message for several handsets is sealed
once per recipient.

Generate the gitignored `include/e2e_keys.h` once:

```sh
python3 tools/generate_keys.py            # devices 1-3
python3 tools/generate_keys.py --devices 4
```

The file holds every pair key, but each `boardN` environment sets
`E2E_DEVICE_ID=N` and compiles only that device's block. Board 1's firmware
contains the 1–2 and 1–3 keys and never the 2–3 key. This was checked by
searching each built `firmware.bin` for all three keys. At boot, a board
stops if its MAC does not match the device ID it was built for.

The build stops with an `#error` if the file is missing or has no block for
the device ID, and the firmware refuses all-zero keys. `--force` replaces the
keys; reflash every board after. Stage 4 replaces these compiled-in keys with
pairing, which derives each pair key on the two boards.

## Configuration

- `include/network_config.h`: device IDs, roles and Wi-Fi station MACs. Board
  1 and board 2 are pre-filled from espnow_encrypt_test; board 3
  (`E0:72:A1:D7:F6:50`) was read with `esptool.py read_mac`. Boards drop
  frames from MACs not listed.
- `include/app_config.h`: `status_led_pin` for an active-high LED with a 330 Ω
  resistor. The default `-1` disables it.

LED: solid while valid messages arrive (3 s timeout), flickering for 400 ms
after a rejected frame, off otherwise.

## Build environments

| Environment | Purpose |
| --- | --- |
| `board1`, `board2`, `board3` | Normal handset for that device ID, over the USB-to-UART bridge |
| `board1-usb`, `board2-usb`, `board3-usb` | Same, with `Serial` on native USB Serial/JTAG |
| `board1-tamper` | Flips one ciphertext bit, then re-signs the hop like a malicious relay |
| `board1-forge` | Seals with keys it made up, like an outsider |
| `board1-replay` | Sends each new frame, then resends the previous frame byte for byte |
| `board3-impersonate` | A legitimate handset (device 3) sends to device 2 claiming to be device 1 |

`pio run` builds `board1` and `board2`.

Upload and monitor as in [espnow_test](../espnow_test/README.md): hold
**BOOT**, tap **RESET**, release **BOOT**, then upload. `BOARD1_PORT` below is
a placeholder; run `pio device list` for the real port.

## Host tests

The protocol logic is tested on a computer against OpenSSL 3. Firmware uses
mbedTLS 2.28 from Arduino-ESP32 2.0.17 behind the same `crypto_backend.h`
interface. Both run the same known-answer vectors: NIST GCM test cases 1–2
and RFC 4231 HMAC-SHA256 case 2. The firmware prints
`Crypto self-test passed` at boot.

```sh
tests/run_host_tests.sh
```

Requires `brew install openssl@3`, or set `OPENSSL_PREFIX`. Covered:

- Round trip and exact header layout
- Plaintext absent from the sealed frame; a new counter gives new ciphertext
- Every single-bit flip of a frame is rejected, both with and without
  re-signing the hop tag
- Redirecting the destination and re-signing is rejected
- A relay verifies with the network key only; the forwarded bytes still open
- Wrong keys and both kinds of forgery are rejected
- Pairwise keys: device 3 claiming to be device 1 is rejected with either of
  its keys, and device 3 cannot open a 1→2 frame
- Length limits: 210-byte maximum payload, short or oversized frames
- Replay: duplicates, late-but-fresh, older than the window, and a forged
  frame not advancing the window

Result on 2026-10-27: `All secure_message host tests passed`. Mutations that
skip the hop tag comparison, skip the replay bitmap, or alter the header
after sealing each made the tests fail.

## Hardware test procedure (stage 1)

Flash board 1 with `board1` and board 2 with `board2` first. For each step,
capture about 15 s of serial from both boards.

1. **Normal exchange.** Both boards print `Crypto self-test passed` and
   `RX ... | "HELLO SE19 #n"` from the other board. TX lines print the full
   frame in hex: the text must not be readable in it.
2. **Tamper.** Flash board 1 with `board1-tamper`:
   ```sh
   pio run -e board1-tamper -t upload --upload-port BOARD1_PORT
   ```
   Expected on board 2: `REJECT ... | bad end-to-end tag` every second and no
   `WARNING`. Board 1 still receives board 2's messages.
3. **Forge.** Flash board 1 with `board1-forge`. Expected on board 2:
   `REJECT ... | bad hop tag`.
4. **Replay.** Flash board 1 with `board1-replay`. Expected on board
   2: `RX` for each new counter, plus `REJECT ... | replay (counter n-1)` for
   each resent frame.
5. **Sender reboot.** Flash board 1 with `board1`, then press RESET on
   board 1. Board 2 keeps accepting; board 1's counter continues from the
   next 1024 block instead of restarting at 1.
6. **Impersonation.** Flash board
   3 with `board3-impersonate`. Expected on board 2: `REJECT ... | bad
   end-to-end tag` every second for board 3's frames claiming source 1, no
   `WARNING`, and RX from board 1 continues. Then flash board 3 with `board3`:
   board 1 should print RX `from 3`, showing board 3's own frames are valid.

## Hardware test procedure (stage 2)

This uses a
controlled broadcast fixture, not promiscuous capture of ordinary unicast:
`board1-capture` seals normal 1→2 messages but broadcasts the ciphertext.
Board 2 authenticates and decrypts normally. Board 3 captures the actual
radio payload using the ESP-NOW callback, without keys or crypto code.
It keeps the first frame and injects it once per second after a one-second
wait. Match its `CAPTURE` counter and hex to board 1's TX and board 2's RX.
A replay result is conclusive only if board 2 accepted that counter first.
If the original was lost, its first replay may legitimately be accepted;
subsequent copies must be rejected. Do not reset board 2 during a run.

### Flashing

Run from this directory. List ports with `pio device list`; substitute each
board's actual port below. Use the UART connector for these environments.
For every upload: hold BOOT, tap RESET, release BOOT, upload, then press
RESET again to start the application. Close serial monitors before uploads.
The physical-MAC checks stop firmware flashed to the wrong board.

```sh
pio run -e board2 -t upload --upload-port BOARD2_PORT
pio run -e board1-capture -t upload --upload-port BOARD1_PORT
pio run -e board3-air-replay -t upload --upload-port BOARD3_PORT
pio device monitor -p BOARD2_PORT -b 115200
```

Start board 2's monitor first and leave it open. Monitor board 3 in a second
terminal (or USB-power board 1 separately when only two USB ports are
available). Reset board 3 after board 2 is receiving. Save 15–20 seconds of
both logs for each mode. Keep board 1 powered and sending throughout.

1. **Over-the-air replay:** board 3 uses `board3-air-replay`, its normal
   allowlisted MAC, and no keys. Expect `Keys held: 0`, a `CAPTURE` line,
   then `TX captured replay`. Board 2 must reject copies as `replay
   (counter N)` from board 3's MAC, while fresh board 1 messages continue.
2. **Spoofed MAC:** reflash only board 3 with `board3-spoof`. It captures
   under its real MAC, then restarts Wi-Fi with board 1's MAC. Each second
   it sends the unchanged capture plus a copy with one ciphertext bit
   flipped. Board 2 must show `replay (counter N)` for the unchanged copy
   and `bad hop tag` for the mutation, both apparently from board 1's MAC.
   Fresh board 1 messages must continue. The duplicate MAC is deliberate;
   judge application RX logs, not Wi-Fi delivery acknowledgements.
3. **Unknown device:** reflash only board 3 with `board3-unknown`. After
   capture it uses `02:53:45:19:00:03`, which is not allowlisted, and sends
   the unchanged capture. Board 2's ten-second `unknown MAC drops` counter
   must increase, with no application RX of that captured counter from
   the unknown MAC. These drops happen before authentication and are not
   included in `rejected`. Fresh board 1 traffic must continue.

```sh
pio run -e board3-spoof -t upload --upload-port BOARD3_PORT
# Complete and save the spoof test before the next upload.
pio run -e board3-unknown -t upload --upload-port BOARD3_PORT
```

All tests require zero receive-queue drops and no negative-build WARNING.
`queued` on board 3 only means local acceptance; receiver logs are required
for a pass. Unknown-MAC drops can include unrelated traffic, so compare
with a baseline while board 3 is off, then observe the increase while it
injects. After testing, restore `board1` and `board3` using the same upload
procedure. No key regeneration or NVS erase is needed.

The three attacker environments compile only `src/attacker.cpp`; they never
include `e2e_keys.h`. The broadcast fixture changes radio delivery only;
the protected destination remains device 2. Normal environments retain
unicast behavior. Passive unicast sniffing and receiver-reboot replay
protection are outside this stage.

## Stage 2 hardware results

2026-10-27, rerun of all three modes with board 1 (`board1-capture`) and
board 2 (`board2`) unchanged. For each mode, board 3 was uploaded and left
in the bootloader. Board 2's serial was opened first, which reset it; board
3's was opened 12 s later, which started the attacker. Both logs were
captured for 36 s with shared timestamps, so board 2 was receiving before
each capture and was not reset during the attack.
Full logs: [`evidence/stage2/`](evidence/stage2/), one file per board
and mode, seconds since the capture started.

Over-the-air replay (`board3-air-replay`):

```text
13.09 [board 2] RX E0:72:A1:D5:64:0C | from 1 | counter 16902 | "HELLO SE19 #16902"
13.14 [board 3] CAPTURE source 1 -> 2 | counter 16902 | 0101010206420000B1B4...
14.12 [board 2] REJECT E0:72:A1:D7:F6:50 | 58 bytes | replay (counter 16902)
30.09 [board 2] Status | accepted 29 | rejected 16 | queue drops 0 | unknown MAC drops 0
```

Board 2 accepted the original 16902 from board 1, then rejected all 23
copies from board 3's MAC as replays while its highest counter was 1–23
ahead, so these were in-window duplicates. It accepted every fresh counter
16890–16925.

Spoofed MAC (`board3-spoof`):

```text
13.23 [board 2] RX E0:72:A1:D5:64:0C | from 1 | counter 17348 | "HELLO SE19 #17348"
13.24 [board 3] CAPTURE source 1 -> 2 | counter 17348 | 01010102C4430000B11D...
14.25 [board 2] REJECT E0:72:A1:D5:64:0C | 58 bytes | replay (counter 17348)
14.25 [board 2] REJECT E0:72:A1:D5:64:0C | 58 bytes | bad hop tag
30.03 [board 2] Status | accepted 29 | rejected 32 | queue drops 0 | unknown MAC drops 0
```

Board 3 sent 22 unchanged copies and 22 mutations under board 1's MAC.
Board 2 rejected exactly 22 as `replay (counter 17348)` and 22 as `bad hop
tag`, with no other rejections, and accepted every fresh counter
17336–17370. The MAC gave no protection; the hop tag and replay window did.

Unknown device (`board3-unknown`):

```text
10.04 [board 2] Status | accepted 9 | rejected 0 | queue drops 0 | unknown MAC drops 0
13.24 [board 3] CAPTURE source 1 -> 2 | counter 17500 | 010101025C440000AFE0...
13.24 [board 2] RX E0:72:A1:D5:64:0C | from 1 | counter 17500 | "HELLO SE19 #17500"
20.04 [board 2] Status | accepted 19 | rejected 0 | queue drops 0 | unknown MAC drops 6
30.09 [board 2] Status | accepted 29 | rejected 0 | queue drops 0 | unknown MAC drops 16
```

The 10 s checkpoint, with board 3 still in the bootloader, is the
attacker-off baseline: zero drops. Board 3 queued 6 frames before the 20 s
checkpoint and 10 between 20 s and 30 s, matching the increases exactly.
No RX or REJECT came from `02:53:45:19:00:03`, and board 2 accepted every
fresh counter 17488–17522.

No run showed a `WARNING` or a queue drop.

| Mode | Build under test | Expected | Observed | Result |
| --- | --- | --- | --- | --- |
| Air replay | `board3-air-replay` | Original accepted; copies rejected `replay` | 16902 accepted, then 23/23 copies rejected in-window; fresh RX unaffected | Pass |
| Spoofed MAC | `board3-spoof` | Copies `replay`, mutations `bad hop tag`, from board 1's MAC | 17348 accepted, then 22/22 `replay` and 22/22 `bad hop tag`; fresh RX unaffected | Pass |
| Unknown device | `board3-unknown` | Dropped before authentication; `rejected` stays 0 | Drops 0 → 6 → 16, matching 6 and 10 sends; no RX from unknown MAC | Pass |

Earlier runs the same day showed the same rejections but had evidence
gaps: the first replay run lacked board 3's `CAPTURE` line and board 2's
original acceptance, and opening board 2's port during the spoof run reset
it, so 15443 was rejected as older than the window rather than as an
in-window duplicate. The rerun above replaces them.

## Test results

2026-10-27, board 3 alone (`board3`): the first flash printed
`esp-aes-gcm: No input supplied` and `Crypto self-test failed`. IDF 4.4's
hardware GCM rejects a null buffer even when its length is zero; OpenSSL
accepts it, so the host tests could not catch this. The mbedTLS backend now
passes a dummy buffer for empty inputs. After reflashing, board 3 printed
`Crypto self-test passed`, `Pair keys held: 2`, and sent counters 1–11 as
54–55-byte frames. Header bytes matched the format (for example
`01 01 03 01 01000000` for counter 1). The ASCII hex of `HELLO` did not
appear in any frame.

2026-10-27, board 1 (`board1`) with board 3 (`board3`), 15 s capture: board 1
received counters 1025–1039 from board 3 as `RX ... | from 3 |
"HELLO SE19 #n"`, decrypted under the 1–3 pair key, with 0 rejected and 0
queue drops. Opening board 3's serial port reset it, and its counter resumed
at 1025 instead of 1, so the NVS counter reservation works across a reboot.

2026-10-27, step 1, board 1 (`board1`) and board 2 (`board2`), 15 s capture
from both: board 2 received counters 2049–2063 from board 1, and board 1
received counters 1–15 from board 2, each decrypted under the 1–2 pair key.
Both boards: `accepted 9 | rejected 0 | queue drops 0` at the 10 s status
line. Board 1's counter started at 2049 because opening its port had reset
it twice before; board 2 started at 1 on its first boot with this firmware.

2026-10-27, step 2, board 1 (`board1-tamper`) and board 2 (`board2`), 15 s:
board 2 printed `REJECT E0:72:A1:D5:64:0C | 57 bytes | bad end-to-end tag`
for all 15 frames from board 1 and `accepted 0 | rejected 9 | queue drops 0`
at the status line, with no `WARNING`. The hop tag was valid (re-signed), so
only the end-to-end tag caught the flipped bit. Board 1 still received
counters 1025–1039 from board 2 with 0 rejected.

2026-10-27, step 3, board 1 (`board1-forge`, boot line `Build mode: FORGE
(negative test)`) and board 2 (`board2`), 15 s: board 2 printed
`REJECT E0:72:A1:D5:64:0C | 57 bytes | bad hop tag` for all 15 frames and
`accepted 0 | rejected 9 | queue drops 0`, with no `WARNING`. Board 1 still
received counters 2049–2063 from board 2 with 0 rejected.

2026-10-27, step 4, board 1 (`board1-replay`) and board 2 (`board2`), 15 s:
board 1 sent counters 5121–5135, each followed by `TX replay of n-1`. Board
2 accepted every new counter (`RX ... | counter 5121` to `5135`) and
rejected all 14 resent frames as `REJECT ... | replay (counter n-1)`.
Status line: `accepted 9 | rejected 8 | queue drops 0`, with no `WARNING`.
The resent frames were byte-identical with valid hop and end-to-end tags, so
only the replay window could reject them.

2026-10-27, step 5, board 1 (`board1`) and board 2 (`board2`): board 1 was
started first, then board 2's serial was captured for 24 s without reset,
and board 1 was reset at 10.2 s. Before the reset board 2 accepted counters
8196–8205; after it, board 1 resumed at 9217 (the next 1024 block) and board
2 accepted 9217–9230. Status lines: `accepted 10 | rejected 0`, then
`accepted 19 | rejected 0`. Had the counter restarted, board 2's replay
window (highest 8205) would have rejected those frames. An earlier attempt
was invalid: after `pio run -t upload` board 1 stayed in the bootloader
(the RTS reset does not work on these boards), so board 2 had no prior
counters. Press RESET after each upload.

2026-10-27, step 6a, board 3 (`board3-impersonate`, powered without serial)
with boards 1 and 2 (normal), 15 s on board 2: every second board 2 printed
`REJECT E0:72:A1:D7:F6:50 | 57 bytes | bad end-to-end tag` for board 3's
frames, and still accepted board 1's counters 10241–10255. Status line:
`accepted 9 | rejected 10 | queue drops 0`, with no `WARNING`. Board 3's
build was confirmed by the tester who flashed it; its serial was not
captured because only two USB ports were available.

2026-10-27, step 6b, board 3 (`board3`), board 1 captured with board 2
powered separately, 15 s: board 1 accepted `from 3` counters 3077–3087 and
`from 2` counters 8214–8224, interleaved, under two different pair keys.
Status line: `accepted 19 | rejected 0 | queue drops 0`. This is the first
run with three handsets on air at once.

| Step | Build under test | Expected | Observed | Result |
| --- | --- | --- | --- | --- |
| 1 | `board1` + `board2` | Two-way RX, text not visible in TX hex | 1→2: 2049–2063; 2→1: 1–15; 0 rejected | Pass |
| 2 | `board1-tamper` | Board 2: `bad end-to-end tag`, no WARNING | 15/15 rejected `bad end-to-end tag`; board 1 RX unaffected | Pass |
| 3 | `board1-forge` | Board 2: `bad hop tag` | 15/15 rejected `bad hop tag`; board 1 RX unaffected | Pass |
| 4 | `board1-replay` | Board 2: new counters RX, resent frames `replay` | 15/15 new accepted; 14/14 resent rejected `replay` | Pass |
| 5 | `board1` + reset | Board 2: RX continues; counter jumps to next block | 8196–8205, reset, 9217–9230; 0 rejected | Pass |
| 6 | `board3-impersonate`, then `board3` | Board 2: `bad end-to-end tag`; then board 1: RX `from 3` | 6a: all of board 3's frames rejected, board 1 RX unaffected; 6b: board 1 RX from 2 and 3, 0 rejected | Pass |

## Limits

- **Receiver reboot:** replay windows are in RAM, so after the receiver
  reboots it would accept recent frames again. Stage 4 fixes this with
  per-boot session keys agreed during pairing, which make old frames fail
  the end-to-end tag.
- **Key storage:** keys are compiled in and readable from flash, so anyone
  with physical access to board 1 can read the 1–2 and 1–3 keys (but not
  2–3). Enabling flash encryption burns eFuses permanently; try it only on a
  spare board.
- **Network key:** shared by every board, so any board can produce a valid
  hop tag. It only gates the relay; sender identity comes from the pair key.
- **Not covered yet:** relay forwarding, passive unicast sniffing, pairing, LoRa and voice. See the stage table.

## Project files

- `include/secure_message.h`, `src/secure_message.cpp`: frame format, seal,
  hop verification, open, self-test.
- `include/replay_window.h`: per-source sliding replay window.
- `include/crypto_backend.h`: AES-GCM and HMAC interface.
  `src/crypto_backend_mbedtls.cpp` (firmware) and
  `tests/crypto_backend_openssl.cpp` (host) implement it.
- `src/main.cpp`: handset firmware, NVS counter, negative-test builds, LED.
- `include/network_config.h`, `include/app_config.h`: devices and settings.
- `include/key_store.h`: pair key lookup for this board's `E2E_DEVICE_ID`.
- `include/e2e_keys.example.h`, `tools/generate_keys.py`: key format and
  generator. `include/e2e_keys.h` is gitignored.
- `tests/secure_message_test.cpp`, `tests/run_host_tests.sh`: host tests.
