# Run 3 replay-rejection investigation

The saved `run3_rate6` logs account for all ten replay rejections on board
1 as duplicate forwards of counters already accepted in the same session.
There is no evidence here of legitimate, previously unseen counters being
incorrectly rejected. No firmware or replay-protection behavior was changed.

## Reproduce the audit

From `tested_parts/e2e_relay_voice_test`:

```sh
python3 tools/investigate_run3_replays.py
```

The script checks the three saved logs and compiles a temporary C++ harness
using the production `include/replay_window.h`. Result: `audit: PASS`, ten
rejections with the captured relay counter sequence, zero with duplicate
counters removed. It asserts the rejected counter identities, not only the
total. This reproduces replay-window behavior, not the radio duplication
or cryptographic verification; packet bytes were not captured.

## Evidence

Affected board-2 counters: 337, 344, 2266, 4485, 4668, 5899, 5942, 6592,
6623, and 6887. Each has one successful `TX_DATA` at board 2, two successful
`FWD` entries at the relay, and one `RX_DATA` at board 1. Each duplicate
pair matches a nearby replay rejection with the same frame size. Counter
337 is text (54 bytes); the other nine are voice-type frames (130 bytes),
which can carry either Opus data or echoes.

For example, counter 337 is forwarded twice at host timestamp 42.06 s;
board 1 records its acceptance and a 54-byte replay rejection at 42.09 s.
The reject log does not contain the counter, so these are correlations,
not direct counter readings from the rejected packets. Host timestamps
come from independently buffered serial ports and do not establish exact
cross-board ordering.

The shared session starts at board 1's 4.95 s handshake. Board 2 had an
earlier session during staggered startup; its earlier counters 1 and 2
are excluded, since counter reuse across sessions is expected.

## Explanations checked

1. **Sender application transmits twice:** not supported. Each affected
   counter has one successful send log, and `send_payload` logs every
   radio send attempt after sealing with the session counter.
2. **Relay retries a held frame twice:** not supported. Its final status
   has `held 0`, `send errors 0`, and `unmatched 0`. The held-frame retry
   path was not exercised. The normal path dequeues and forwards each
   received-frame record once.
3. **Duplicate delivery into the relay receive path:** consistent with
   the logs and code. Two in-flight forwarding records require two
   successful relay send calls. With no held retries, the normal code
   path implies duplicate receive records. There is no receive-callback
   trace or over-the-air capture to establish why they were duplicated.

`session::link::open` authenticates and decrypts before `finish` checks the
replay window. Thus these replay results occur after successful message
authentication. All recorded voice streams still delivered every frame.
The replay window correctly prevents duplicate application delivery.

## Remaining boundary

Radio retransmission/driver duplicate delivery is a possible explanation,
not a proven root cause. The saved logs cannot distinguish its mechanisms.
If deeper radio diagnosis is needed, capture relay ingress sequence IDs,
receive timestamps and frame fingerprints, carry those IDs into send
records, and correlate with an over-the-air capture. Keep instrumentation
out of the Wi-Fi callback's serial output to avoid perturbing timing.

No security relaxation or counter reset is warranted by this evidence.
