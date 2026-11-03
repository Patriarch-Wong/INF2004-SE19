# Two-board integration edge cases — 2026-11-03

Tested the existing handset firmware on physical boards 1 and 2 over USB
serial at 921600 baud. Repository HEAD: `adbfcb6`. No firmware was changed
or flashed, and the running binary has no commit identifier in its boot log.
Both identified themselves as handset integration firmware with connected-only
hello commands. Channel 11, 1 Mbps, redundancy on, 300 ms playout buffer.
Board 3 was absent. These are serial-driven hardware tests, not a manual
assessment of the display, physical buttons, or audible quality.

## Result

One new failure: **`connect 258` calls board 2 instead of rejecting the ID**
(C10). `src/handset.inc` narrows the parsed unsigned integer to `uint8_t`
before `place_call` validates it. At 11.18 s in the board 1 log the command
starts a call to board 2; board 2 logs the matching incoming request at
11.10 s. Small timestamp differences reflect serial buffering, not exact
radio latency. The request is cancelled at 12 s. Firmware was unchanged during these hardware runs. See the fix follow-up below.

All host suites passed: secure message, session, connection, voice link,
audio DSP, stream checker, and shared-key consistency. Host connection tests
include lost answers/hangups, one-way loss, busy peers, stale messages,
reboots and clock wrap; those fault injections were not performed over RF.

## Main hardware run

Raw logs: [board 1](edge_20261103_b1.txt), [board 2](edge_20261103_b2.txt).
Replay schedule: [edge_20261103_run.py](edge_20261103_run.py).
Run from the project directory with
`~/.platformio/penv/bin/python evidence/edge_20261103_run.py`.
Opening the serial ports resets these boards; the schedule also explicitly
reboots board 2 twice and temporarily changes its radio channel.

| Edge case | Evidence and outcome |
| --- | --- |
| Voice/text outside a call | Both reject `hello` and `talk 1`, at 4–5 s and hello again at 159 s. Pass. |
| Accept/decline without a request | Both reject at 6–7 s without changing state. Pass. |
| Invalid target | Board 1 rejects 0, self (1), and unprovisioned 4. **258 wraps to 2: fail.** |
| Near-simultaneous connect | At 14 s board 2's request arrives before board 1 processes its dial. Board 1 refuses the second dial as already asked. This does not exercise true glare. |
| Simultaneous PTT | At 26 s both start; board 2 yields, board 1's 99 frames arrive intact. Pass. The 20 s attempt accepted the pending call on board 1, so only board 2 talked. |
| Text while voice plays | At 33 and 38 s, text arrives in both directions; both 150-frame test streams have matching CRCs and intact=yes. Pass. |
| Disconnect mid-PTT, immediate reconnect | Board 2 hangs up at 44 s, redials at 44.5 s; connected by 45.43 s. Reverse voice starts at 46 s, all 100 frames intact. Pass for immediate reuse. Old stream finished naturally: `RX cut` path not exercised. |
| Radio loss during call | Board 2 moves to channel 1 at 52 s; boards end call at 53.52/53.60 s. Both UI snapshots show grey peer dots. Offline connect refused at 58 s. Pass. This simulates RF isolation, not unplugging power. |
| Radio recovery | Board 2 returns to channel 11 at 60 s; board 1 marks it online at 62.32 s; call succeeds at 69 s. Pass. |
| Reboot while connected | Board 2 reboot at 71 s; board 1 detects new session and ends call at 72.91 s. Both idle afterward. Pass. |
| Reboot while ringing | Request at 80 s, board 2 reboot at 81 s; board 1 ends request as unreachable at 83.25 s. Rebooted board remains idle, no stale request. Pass. |
| Decline and cancel | Decline at 92 s reaches both boards; cancel at 97 s clears both. Pass. |
| No answer | Request at 101 s ends at 131.11 s as no answer; receiver clears it. Pass. |
| Voice after recovery | Reverse-initiated call at 135 s; each direction sends 250 test frames, all intact with matching CRCs. Pass. |
| Final cleanup | Both idle on menu, channel 11; held buttons released, feedback filters reset with `r`. |

`check_streams.py` passes in both directions:
[1 → 2](edge_20261103_b1_to_b2.json),
[2 → 1](edge_20261103_b2_to_b1.json).
Eight delivered streams total 1,138 frames, with no missing or late frames,
matching CRCs, and zero send errors. A ninth stream yielded before sending
any frames and is correctly excluded from playback. These short runs do not
resolve the intermittent late burst previously recorded as C9.

Existing audio caveats reproduced: most streams report one underrun (C8);
the interrupted stream reports three despite intact packet delivery. Test
signals trigger feedback notches (C7); `r` clears them after testing.

## Follow-up: real simultaneous dialing and hello throttling

Logs: [board 1](edge_followup_20261103_b1.txt),
[board 2](edge_followup_20261103_b2.txt).
Replay: [edge_followup_20261103_run.py](edge_followup_20261103_run.py),
run with the same PlatformIO Python as the main schedule.

Three near-simultaneous dial attempts at 5, 13 and 21 s. The first becomes
a normal request. In the latter two, **both boards enter connecting with
different call IDs**; board 2 then yields to board 1's call ID and rings.
Acceptance succeeds and both report the same connected ID. True glare
therefore passed twice on hardware. Near-simultaneous disconnects also
leave both idle, without restoring a stale call.

Each board sends a hello, then tries again about 0.25 s later, in four calls.
All eight early repeats log `TEXT wait: one hello per second` and produce
no extra receive. A further hello after 1.5 s succeeds each way. Each board
has exactly five sent and five received text messages. At 34.6 s the UI
snapshot shows the correct peer's banner. Final `link` at 37.11 s reports
both idle; all serial-held buttons are released. No resets after startup.

## Limits

No selective packet loss, forged RF frames, true power removal, third-board
busy case, or 49-day hardware clock wrap was injected. No physical button or
acoustic checks were performed. Initial boot bytes are garbled at the capture
baud; subsequent command/result lines are readable. No crash was observed;
board 2's two mid-run resets were commanded.

## C10 fix follow-up

The serial `connect` command now parses decimal digits with a board-slot
range check before converting to `uint8_t`. It rejects `258`, signed values,
values larger than a machine integer, zero, and trailing junk without calling
`place_call`. Valid IDs still go through the existing self-call, provisioned
MAC, presence and call-state checks.

`tests/command_parser_test.cpp`, included in `tests/run_handset_tests.sh`,
covers the configured ID range, whitespace, leading zeros, the original
`258` reproduction, integer overflow strings, signs and malformed arguments.
Validation: the command-parser and existing handset host suites pass;
`pio run` successfully builds all five board environments. Hardware
confirmation awaits the user's normal BOOT/RESET upload procedure.
After flashing, send `connect 258` on board 1: expect
`LINK refused: invalid board ID` and no request on board 2. Then send
`connect 2` and accept on board 2 to confirm ordinary calls still work.

## C10 hardware retest before upload

A subsequent hardware probe found the old parser still running on both
boards. [Board 1 log](c10_probe_20261103_b1.txt): `connect 258` at 4.16 s
started a call to board 2. [Board 2 log](c10_probe_20261103_b2.txt) records
the matching request at 4.14 s; `connect 257` was handled as an attempt to
dial while already asked, rather than an invalid ID. Both were idle after
cancellation at 6 s. This does not test the fixed firmware.

The command-parser and handset host suites were rerun and passed. Hardware
verification remains pending upload of the newly built board1 and board2
firmware using the manual BOOT/RESET procedure documented in the README.

## C10 post-flash hardware verification

**Passed on both boards after the user flashed the fix. C10 is closed.**
This supersedes the pending hardware status above. Channel 11, 1 Mbps,
300 ms buffer, serial at 921600 baud; no additional firmware edits.

- Each board rejects all 18 invalid inputs: `258`, `257`, `514`, `-254`,
  `-255`, `+2`, `0`, `6`, `255`, `256`, `4294967298`,
  `18446744073709551618`, a 40-digit number, `2x`, `2 3`, `2.0`, `0x2`,
  and a missing argument. All report `LINK refused: invalid board ID`;
  neither starts a call or receives an unintended request.
- Self-call and unprovisioned slots 4/5 are rejected by the existing
  checks. Absent board 3 is rejected as offline.
- `connect 2` from board 1 connects normally. `connect 258` on both boards
  while connected is rejected without ending or replacing the call.
- Board 2 then initiates a new call with whitespace and leading zeros
  (`connect \t001 `); board 1 accepts.
- Hello text arrives in both directions in both calls. Each board sends
  one two-second test stream: 100/100 frames received, matching CRCs,
  `intact=yes`, zero missing/late frames and zero send errors. The existing
  one-underrun-per-stream observation (C8) remains; audible quality was not
  assessed.
- Both calls disconnect normally. Both boards finish idle on the menu,
  peer dots green, with feedback filters reset using `r`.

Artifacts: [board 1 log](c10_fixed_20261103_b1.txt),
[board 2 log](c10_fixed_20261103_b2.txt),
[replay schedule](c10_fixed_20261103_run.py),
[log assertions and stream checker](c10_fixed_20261103_check.py),
[machine-readable results](c10_fixed_20261103_results.json).
Run the checker with `python3 evidence/c10_fixed_20261103_check.py` from
the project directory. Both boards pass every check.
