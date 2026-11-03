# Handset integration: issues and planned fixes

Problems found in `handset_integration_test`, mostly about running more
than two boards. Started 2026-11-03. Hardware tests (H1–H13) are in
`README.md`.

Status: `open`, `in progress`, `done (not on hardware)`, `done`,
`won't fix`. Update the row when an item changes, and add the commit or
the evidence.

## A. Fixed in code, waiting for hardware tests

| ID | Problem | Fix | Test | Status |
| --- | --- | --- | --- | --- |
| A1 | **Both press OK at once.** Neither board has heard the other yet, so both start sending and each drops the other's voice. Both screens stay on `TALKING` and nobody hears anything. | The board that hears the other's voice while sending applies `voice_link::yields_floor`: the higher random stream ID keeps talking, and on a tie the lower board ID does. The other board stops at once, logs `TX_YIELD` and plays the winner from its first word. | H11 | done, `fb251ec`; passed on [hardware](evidence/hardware_2026-11-03.md) (serial buttons) |
| A2 | **Menu dot stays green for boards that are off.** A session is made once at boot and never expires, and nothing was sent while idle. | Presence ping every 2 s to each board with a session. A board is online if anything from it opened in the last 6.5 s. The menu dot, Status page and `Connect` follow this. The log shows `PEER N online/offline`. | H12 | done, `fb251ec`; passed on [hardware](evidence/hardware_2026-11-03.md) (channel change for unplug) |
| A3 | **A request outlives the board it asks.** Ask board 2, then board 2 goes off: board 1 keeps resending for up to 30 s. If board 2 reboots within that time, it rings straight away for a request nobody is waiting on. | `check_call_peer` ends a request or connection when the other board goes offline or makes a new session, which means it rebooted. A request shows `Not reachable`, a connection shows `Connection lost`. | H13 | done, `edde973`; passed on [hardware](evidence/hardware_2026-11-03.md) (offline and reboot) |
| A4 | **Retrying offline boards too often** (was B1). Hellos went every 1 s forever to listed boards that were off, and presence pings every 2 s to offline boards. The radio retries each one, and voice frames wait behind it. | 10 hellos a second apart, then one every 15 s (`SESSION 3 not answering`). Presence pings to offline boards every 5 s. A hello from the peer while this board is initiating triggers its own hello at once, so the lower ID doesn't make a rebooted board wait. | H15 | done, `edde973`; passed on [hardware](evidence/hardware_2026-11-03.md) |
| A5 | **Accidental accept** (was B3). An OK meant for the menu, or pressed just after the switch, accepted a request the user hadn't seen. | The request screen ignores OK and CANCEL for 500 ms after it appears (`BUTTON ignored`). | H14 | done, `edde973`; passed on [hardware](evidence/hardware_2026-11-03.md) (serial buttons) |
| A6 | **Leftover streams after a reconnect** (was C2). For about 2 s, the last connection's stream blocked talking or dropped the new board's first words. | When a connection starts, a stream from the last one is dropped (`RX cut`, no roger beep), and draining for it stops. | H16 | done, `edde973`; sending side passed on [hardware](evidence/hardware_2026-11-03.md). The receiving-side cut (`RX cut`) was not triggered in the run |
| A7 | **Link loss took up to 6 s** (was C5). | ESP-NOW's send callback counts frames the peer's radio never acknowledged. 3 in a row, with nothing acknowledged for 2 s, means the board is offline: a connection ends as `Connection lost` and a request as `Not reachable`, within about 2–3 s. | H6 | done, `edde973`; passed on [hardware](evidence/hardware_2026-11-03.md): 1.6–2.0 s |
| A8 | **Serial `connect` / `accept` ended at once** (found on hardware, `evidence/h_run1_*`). Serial commands stamped state with `millis()`, a few ms after the loop's `now`; `now - since` then wrapped to 49 days, so asking ended as `no answer` and accepting as `link lost` within the same pass. The same mistake made `PEER 2` flicker offline after a handshake, could repeat the stream end message every loop pass instead of every 100 ms, and could briefly show a huge "heard N s ago". | Every state timestamp now uses the loop's `now` (serial commands, handshake, stream end, receive end). `call.cpp`'s timeouts and `peer_online` compare signed, so a time slightly in the future counts as none passed. Host test `test_action_after_tick_time` fails without the fix. | H3 by serial | done; passed on [hardware](evidence/hardware_2026-11-03.md) (runs A and B) |
| A9 | **Menu dots stale on return** (found on hardware, run B). While another screen shows, the dots are not updated, so the menu was first drawn with old colours (green for a board that had gone offline), until the next refresh 100 ms later. | `draw_menu` updates every dot before drawing. | `ui` after `Not reachable` | done (not on hardware) |

## B. Open: affects calls with more boards

| ID | Problem | Effect | Planned fix | Status |
| --- | --- | --- | --- | --- |
| B2 | **A busy board looks free.** The green dot only means online. | Calling a board that is in another call fails with `Busy` after a round trip. | Add a status byte to the presence ping (free / asking / in a call) and show `In call` on the menu. | open |
| B4 | **Three boards asking in a circle.** 1 asks 2, 2 asks 3, 3 asks 1 at the same moment. The two-board tie-break doesn't cover it. | All three get `Busy`. Rare, and a retry fixes it. | Random short retry delay, or keep as is. | open |
| B5 | **Missed requests aren't shown.** A third board asking while you are connected, or already being asked, gets `Busy`. | The user never learns someone tried. | A short `Board N tried to connect` notice after the connection. | open |

## C. Open: smaller edge cases

| ID | Problem | Effect | Planned fix | Status |
| --- | --- | --- | --- | --- |
| C1 | **Collision with a very quick release.** After A1, a few frames the losing board had already queued can arrive after the winner has released. | Only if the winner released within a few milliseconds: it plays a short scrap, waits about 1.5 s, logs `intact=no` and beeps. | Not worth it: ignoring that stream would hide a real talker when only one direction works. | won't fix (unless seen) |
| C3 | **A session rebuilt for another reason ends the call.** After about 5 s of frames failing, the board rebuilds the session, and A3 treats that like a reboot. | The call ends, but a link failing that long drops anyway (6 s). | None needed. | won't fix |
| C4 | **Offline takes up to 6.5 s to show on the menu.** | A board that just switched off stays green until its pings stop or three of ours go unacknowledged (A7). A3 covers requests sent in that window. | Shorter ping period if needed (more airtime, see A4). | open |
| C6 | **A board never seen before can take 15 s to show** after coming into range (A4's slow hellos; reboots are still fast). | Slower first contact when a board walks into range without rebooting. | Fine for now. | won't fix |
| C7 | **The howl guard notches the test signal** (hardware run A). The speaker's feedback guard treats the `test` signal's tones as howl, and both boards added the same 5 notches (438, 516, 594, 688, 1820 Hz). | The notches stay and filter later speech until `r` or a reboot. Only after `test`, so not in normal use, but it skews voice tests that follow a test-signal run. | Skip the howl guard for test-signal streams, or run `r` after each `test`. | open |
| C8 | **One underrun per stream** (hardware run A: `underruns=1` on most streams). | Probably a single 20 ms gap at the start or end of playback. Not audible in the runs, not yet checked. | Log where in the stream the underrun falls. | open |
| C9 | **A one-off late burst** (hardware H9: 1 of 20 streams). 8 of 200 frames arrived in order but after their playout slot, so they were neither missing nor asked for again. That points to a single delay of about 160 ms beyond the 300 ms buffer. A hello to the absent board 3 fell inside that stream, but two 60 s streams crossing 7 such hellos had 0 late frames, so that isn't confirmed. | One short garble in about 2 minutes of talk. | Log when late frames arrive, to tell an air delay from a loop stall. Or raise the default playout buffer to 400–500 ms (delay is acceptable). | open |

## D. Test setup

| ID | Problem | Next step | Status |
| --- | --- | --- | --- |
| D1 | **Board 3 still runs `e2e_relay_voice_test`**, which has a different network key. Its MAC is in slot 3, so boards 1 and 2 log `REJECT` for its frames and keep sending it hellos (every 15 s since A4). | Flash board 3 with `board3` from this project, or keep it off during tests. | open |
| D2 | **Boards 4 and 5 not provisioned.** Slots 4 and 5 have zero MACs. | Add their MACs to `network_config.h` when the boards are wired. | open |
| D3 | **Two calls at once (1–2 and 3–4).** Expected to fit (about 10% of airtime per talker at 1 Mbps), but untested. | Hardware test once four boards are wired. | open |

## Not covered here

- "All" (broadcast) mode: planned, unencrypted by decision; see `README.md` Limits.
- LoRa: next stage.
