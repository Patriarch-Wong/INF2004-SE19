# ESP-NOW pairing, pair keys in NVS and session keys

Stage 4 of the end-to-end security tests in
[e2e_security_test](../e2e_security_test/README.md). In stages 1–3, every
pair key came from `generate_keys.py` and was compiled into the firmware.
Here two handsets make their pair key over the air when the users pair them,
keep it in NVS, and derive a new session key from it after every boot. The
session keys fix the receiver-reboot replay listed under stage 1's limits.

Status: host tests pass, all environments build, and six of seven hardware
tests passed on 2026-10-27. The BOOT button test (7) was partial: start,
accept and reject by button all worked, but no pairing was completed by
button alone. The same pairing path passed through the serial commands.

## What is reused

The frame format, AES-GCM/HMAC crypto and replay window are unchanged; see
[Protected message
format](../e2e_security_test/README.md#protected-message-format).
`include/secure_message.h`, `src/secure_message.cpp`,
`include/replay_window.h` and `tests/secure_message_test.cpp` are copied
byte for byte. `crypto_backend` gains X25519 (mbedTLS on the boards, OpenSSL
in the host tests).

## Keys

| Key | Where it comes from | Held by | Purpose |
| --- | --- | --- | --- |
| Network | `tools/generate_keys.py`, compiled in | Every handset | Hop tag, as in stages 1–3 |
| Pair key, one per paired handset | Pairing (X25519 + code comparison) | NVS on the two paired handsets | Only to authenticate the session handshake and derive session keys |
| Session key | Handshake after every boot | RAM on the two handsets | AES-128-GCM for text and voice |

```sh
python3 tools/generate_keys.py   # include/e2e_keys.h: network key only
```

The firmware has no pair keys compiled in; boards start unpaired. Pair keys
are stored in NVS namespace `pairs` as `peer<N>`, which uploads do not
overwrite. Logs show 32-bit fingerprints (HMAC of the key), never keys, so
two boards can be seen to hold the same key.

## Pairing

Numeric comparison, as in Bluetooth LE Secure Connections
(`include/pairing.h`, `src/pairing.cpp`):

1. Each handset makes a fresh X25519 key pair and a 16-byte nonce, and
   broadcasts an **offer** with its public key.
2. The lower device ID is the initiator (I), the other the responder (R). R
   sends a **commit**: an HMAC of its nonce and both public keys.
3. I reveals its nonce, then R reveals its nonce, and I checks R's nonce
   against the commitment.
4. Both compute the six-digit code from both public keys and both nonces,
   and the pair key with HKDF-SHA256 from the X25519 shared secret (salt:
   both nonces; info: both IDs and both public keys).
5. Each user checks that the other device shows the same code and accepts
   (`yes` or BOOT). Each side then sends a **confirm** tag made with a key
   derived from the same secret. A handset stores the pair key only once
   its own user has accepted and the peer's tag is valid.

Why it resists a man in the middle: X25519 alone does not say who is on the
other end, so an attacker could swap in its own public key in both
directions. The code catches that. The responder is bound to its nonce
before it sees the initiator's nonce, and the initiator reveals its nonce
before it sees the responder's, so an attacker cannot steer the two codes
to match. It succeeds with probability 1 in 10⁶ per attempt, and only if
both users accept. As a second check, a handset that sees two different
public keys for the peer it chose stops the attempt (`conflicting public
keys for the peer`).

All pairing messages are broadcast, carry the normal header and hop tag
(control type 3, `include/control_frame.h`), and are resent every 250 ms
until answered. An attempt times out after 60 s. A rejection or timeout
sends an **abort** so the other side stops too.

Controls: `pair` (any handset) or `pair N`, then `yes` or `no`. BOOT: a
short press starts pairing with any handset, or accepts a shown code; a long
press (1.5 s) rejects.

## Session keys

After every boot a handset starts a handshake with each paired handset
(`include/session.h`, control type 4):

| Step | Direction | Body |
| --- | --- | --- |
| hello | I → R | I nonce |
| reply | R → I | I nonce, R nonce, HMAC tag |
| confirm | I → R | I nonce, R nonce, HMAC tag |

The tags use a key derived from the pair key, so only the other holder can
answer. Session key = HKDF(salt: both nonces, secret: pair key, info: both
IDs). Text and voice frames are sealed under it with counters starting at 1.

- **Receiver reboot:** the rebooted handset makes a new nonce, so frames
  recorded earlier fail the end-to-end tag. The replay window can stay in
  RAM.
- **No NVS counter:** every session has a new key, so a counter that starts
  at 1 never repeats a GCM nonce under one key. Each side's own nonce is
  enough to make the key new, even if the peer's RNG repeats.
- **Replayed handshake:** the responder keeps its working session until it
  gets a valid confirm, or a data frame that opens under the new key. A
  replayed hello gets a reply but changes nothing.
- **Both start at once:** the lower device ID stays initiator.
- **Lost step:** if a paired handset's frames keep failing for 5 s with
  nothing accepted, the receiver starts a new handshake.

## Build environments

| Environment | Board | Purpose |
| --- | --- | --- |
| `board1`, `board2`, `board3` | 1, 2, 3 | Handset |
| `board1-replay-old-session` | 1 | Keeps the last frame of its session with device 2; once device 2 has started a new session, resends it every second |
| `board3-pair-mitm` | 3 | Network key only. Poses as device 2 to device 1 and as device 1 to device 2 during pairing, and accepts every code |

`pio run` builds the three handsets. All use the UART connector.

Serial commands: `pair`, `pair N`, `yes`, `no`, `unpair N`, `unpair all`,
`keys`, `reboot`. `unpair` erases the key on that board only; the other
board must unpair too.

## Host tests

```sh
tests/run_host_tests.sh
```

Requires `brew install openssl@3`, or set `OPENSSL_PREFIX`. Three programs:

- `secure_message`: the stage 1–3 tests, unchanged.
- `pairing`: control frames; HKDF and X25519 known answers (RFC 5869, RFC
  7748); a pairing over a simulated broadcast medium with the same code and
  key on both sides; the code depends on both public keys and both nonces;
  the pair key recomputed independently from X25519, nonces and transcript;
  pairing with any peer; 40% frame loss; rejection and abort; conflicting
  offers; a full man in the middle (the attacker learns each code, but the
  two victims' codes differ; never equal in 200 runs); a responder nonce
  that does not match its commitment; a bad confirm tag; a low-order public
  key; stale and misaddressed messages ignored.
- `session`: handshake and data; receiver reboot with either side having
  started the old session; old frame after a replayed hello on a rebooted
  receiver; a peer with a stuck RNG; replayed hello, reply and confirm;
  lost confirm; resent hello; simultaneous hello; an impostor with the
  wrong pair key.

Result on 2026-10-27: all three pass. Each of these mutations made a test
fail: skipping the commitment check, the conflict check, the confirm tag
check or the reply/confirm tag checks; completing on one acceptance;
leaving either nonce out of the session key, the responder's nonce out of
the code, or the X25519 secret or nonces out of the pair key; promoting no
pending session; a hello that replaces the working session; no tie-break;
and a new reply nonce for a resent hello.

## Hardware test procedure

The boards cannot enter the bootloader over USB. For each upload: hold BOOT,
tap RESET, release BOOT, upload, then press RESET. NVS survives uploads, so
pairings persist across reflashing.

```sh
pio run -e board1 -t upload --upload-port /dev/cu.usbmodem5B420192981
pio run -e board2 -t upload --upload-port /dev/cu.usbmodem5B5F0215971
pio run -e board3 -t upload --upload-port /dev/cu.usbmodem5B420192791
```

`tools/capture_logs.py` captures several boards with shared timestamps and
sends serial commands at set times (`--send LABEL@SECONDS=TEXT`). The log
records each as `>>> TEXT`, and the firmware echoes it as `CMD`. Opening a
port resets that board, so ports open at staggered times
(`LABEL=PORT@SECONDS`). Logs go to `evidence/stage4/`. Run
`python3 tools/find_keys.py evidence/stage4/*.txt` before committing.

```sh
PY=~/.platformio/penv/bin/python
B1=/dev/cu.usbmodem5B420192981
B2=/dev/cu.usbmodem5B5F0215971
B3=/dev/cu.usbmodem5B420192791
```

1. **Pair 1–2.** `$PY tools/capture_logs.py pair-12 board1=$B1 board2=$B2@2
   board3=$B3@4 --duration 40 --send "board1@8=pair 2" --send "board2@8.5=pair
   1" --send board1@14=yes --send board2@17=yes`. Pass: same code and
   pair-key fingerprint on both; a session with the same ID on both; RX
   both ways from counter 1; board 3 stays unpaired.
2. **NVS and new session per boot.** `$PY tools/capture_logs.py reboot
   board1=$B1 board2=$B2@2 --duration 45 --send board2@10=reboot --send
   board1@25=reboot`. Pass: `Pair keys in NVS: 1` with the same fingerprint
   after each reboot, a new session ID each time, counters from 1, nothing
   rejected.
3. **Old frames after the receiver reboots.** Board 1 with
   `board1-replay-old-session`. `$PY tools/capture_logs.py old-session
   board2=$B2 board1=$B1@3 --duration 45 --send board2@15=reboot`. Pass:
   board 2 accepted the original frame before rebooting, rejects every
   replay as `bad end-to-end tag`, and keeps accepting fresh frames.
4. **Rejected and attacked pairing.**
   a. Boards 1 and 3 (normal): `pair 3` / `pair 1`, `yes` on board 1, `no`
      on board 3. Pass: board 3 `rejected on this device`, board 1 `peer
      aborted`, no key on either.
   b. Board 3 with `board3-pair-mitm`: `$PY tools/capture_logs.py mitm
      board3=$B3 board1=$B1@2 board2=$B2@3 --duration 30 --send
      "board1@8=pair 2" --send "board2@8=pair 1"`. Pass: both stop with
      `conflicting public keys for the peer` and keep their old key. Then
      with board 2 not pairing, `pair 2` on board 1 alone: board 1 shows a
      code but board 2 shows none, so the tester enters `no`.
5. **Second pairing and unpair.** Boards 1 and 3: `pair` on both, `yes` on
   both, then `unpair 3` on board 1. Pass: board 1 holds two keys with
   separate sessions; after unpairing, board 3's frames are rejected `no
   pair key for source`.
6. **Keys in images and logs.** `python3 tools/find_keys.py
   .pio/build/*/firmware.bin evidence/stage4/*.txt`. Pass: images hold the
   network key only; logs hold no key; `48454C4C4F` (`HELLO`) never appears
   in logs.
7. **BOOT button.** Short-press BOOT on two boards, short-press each again
   once the codes match. Pass: as test 1. Then start again and long-press
   on one board: as test 4a.

Every test: no queue drops, no resets except where the test says so.

## Results

2026-10-27, all three boards on USB. Logs are in `evidence/stage4/`. Before
test 1, no board had a pair key.

Test 1 (`pair-12`): both boards showed `PAIR code 862972`, and both stored
pair key `48FF15EF`. Board 2 accepted at 17.19 s and stored first; its
first hello reached board 1 before board 1 had the key (`REJECT ... no pair
key for source`, the only rejection). Board 1 stored at 17.35 s and became
initiator, and both reached session `2BD4CA86` at 17.45 s. Board 1 accepted
counters 1–23 and board 2 counters 1–22, with no gaps. Board 3 printed
`sessions 1=unpaired 2=unpaired` throughout.

Test 2 (`reboot`): every boot printed `Pair keys in NVS: 1` and `pair key
48FF15EF`, and every reboot made a new session: `0D411812` and `1C43E48D`
as the capture opened the ports, `4AA1E914` after board 2's `reboot` at
10 s, `5CCE65AB` after board 1's at 25 s. The rebooted board was initiator
each time. Counters restarted at 1 in every session; both boards ended with
`rejected 0 | queue drops 0`.

Test 3 (`old-session`): board 2 accepted `counter 12` in session `08129ECC`
at 15.10 s, then rebooted. Board 2 started session `68D885A4` at 16.02 s,
and from 16.12 s board 1 resent that counter-12 frame every second, 29
times. Board 2 rejected all 29 as `bad end-to-end tag` and accepted 29 fresh
frames, counters 1–29 of the new session. Until 27.15 s the new session had
not reached counter 12, so a replay window reset by the reboot would have
accepted the old frame; only the new key stopped it. Board 3 was running
`board3-pair-mitm` but, judging by `own ID claims 0`, not transmitting; it
does not affect data frames.

Test 4a (`reject`): both boards showed `283376`. Board 1 accepted; board 3
entered `no` and printed `PAIR FAILED | rejected on this device`, and board
1 printed `PAIR FAILED | peer aborted` at the same moment (12.09 s and
12.12 s on the two ports). `keys` on both: device
3/1 `not paired`. At 0.93 s board 1 rejected one frame from board 2 as `bad
end-to-end tag`: board 2 sent it under the old session just before board
1's new session reached it.

Test 4b (`mitm`): board 3 printed `Keys held: network only; no pair keys`
and offered as device 1 and as device 2. After `pair 2` and `pair 1` at
8 s, board 1 printed `PAIR FAILED | conflicting public keys for the peer`
at 8.33 s and board 2 at 8.26 s. The attacker reached a code with board 1
(`812032`) before board 1's abort ended it, and completed no pairing. Both
boards kept pair key `48FF15EF` and their session. `own ID claims` rose
about 4 per second on both, from the attacker's offers under their IDs. The
9 rejections per 10 s on board 2 are board 1's old-session replays from
test 3's build.

Test 4b, board 1 alone (`mitm-alone`): board 1 printed `PAIR code 535585
with device 2`, and the attacker printed the same code. Board 2 was not
pairing and showed no code, so `no` was entered; board 1 printed `rejected
on this device` and kept `48FF15EF`. Had the tester accepted, board 1 would
have paired with the attacker.

Test 5 (`pair-13`): `pair` (any handset) on boards 1 and 3; both showed
`319631` and stored `FED945CA`. Board 1 then had two sessions, `DC716D9D`
with device 2 and `74210056` with device 3, receiving from both. After
`unpair 3` at 25.09 s, board 1 printed `UNPAIRED device 3 | key erased from
NVS` and rejected every later frame from board 3 as `no pair key for
source` (19 frames). Board 3's own key was erased afterwards with `unpair
all`.

Test 7 (`boot-button`), partial: every press below was a physical BOOT
press. A short press started pairing on both boards (board 2 at 35.56 s,
board 1 at 53.27 s), and both showed `311888`. Board 2 registered its next
press as long, printed `rejected on this device`, and board 1 stopped with
`peer aborted`. Second attempt: short presses on both, both showed
`792883`, and a short press on board 2 accepted it (`PAIR accepted here`).
Board 1 registered no press while its code was shown, so the pairing did
not complete; a long press on board 2 then rejected it and both stopped
with nothing stored. Presses during searching or exchanging are ignored
without a log line, so the log cannot show whether board 1's other presses
were missed or ignored.

Test 6: `tools/find_keys.py` reported `network` for all five firmware
images and `no keys` for all 16 logs. `48454C4C4F` appears in no log.

| # | Test | Result |
| --- | --- | --- |
| 1 | Pair 1–2 over serial | Pass |
| 2 | Keys in NVS, new session per boot | Pass |
| 3 | Old frames after the receiver reboots | Pass: 29/29 rejected `bad end-to-end tag` |
| 4 | Rejected pairing; man in the middle | Pass: abort on rejection; both victims stop on conflicting keys |
| 5 | Second pairing and unpair | Pass |
| 6 | Keys in images and logs | Pass |
| 7 | BOOT button | Partial: start, accept and long-press reject work by button; no pairing completed by button |

## Limits

- **The code check is only as good as the user.** If both users accept
  without comparing, or accept a code when the other device shows none
  (test 4b, board 1 alone), the handset pairs with the attacker.
- **Network-key holders can disrupt pairing.** Conflicting offers, a false
  commit or an abort stop an attempt (denial of service), but cannot make
  the two codes agree.
- **Key storage:** NVS is not encrypted. Anyone with physical access can
  read a board's pair keys. NVS encryption needs flash encryption, which
  burns eFuses.
- **Unpair is one-sided:** the other board keeps its key and keeps trying
  to start sessions until it is unpaired too.
- **Network key** is still compiled in and shared, as in stages 1–3.
- **Not covered:** relayed pairing and sessions (control frames carry the
  routing header and hop tag, but the relay was not run here), LoRa, voice.

## Project files

- `include/pairing.h`, `src/pairing.cpp`: pairing state machine; no radio
  or timing code, so it runs in the host tests.
- `include/session.h`, `src/session.cpp`: session handshake, session keys,
  data seal/open and replay window.
- `include/control_frame.h`, `src/control_frame.cpp`: pairing and session
  messages in the protected frame header with a hop tag.
- `include/kdf.h`, `src/kdf.cpp`: HKDF-SHA256, truncated HMAC,
  fingerprints, known-answer self-test.
- `include/radio.h`, `src/radio.cpp`: Wi-Fi/ESP-NOW setup, MAC allowlist,
  receive queue.
- `src/main.cpp`: handset: pair store in NVS, commands, BOOT button,
  sessions, text traffic, old-session replay build.
- `src/pairing_mitm.cpp`: pairing attacker.
- `include/network_config.h`, `include/app_config.h`: devices, timings,
  BOOT pin.
- `include/network_key.h`, `include/e2e_keys.example.h`,
  `tools/generate_keys.py`: network key.
- `tools/capture_logs.py`: multi-board capture with timed commands.
- `tools/find_keys.py`: key search for images and logs.
- Copied from stages 1–3: `secure_message`, `replay_window`,
  `crypto_backend` (plus X25519), `tests/secure_message_test.cpp`.
