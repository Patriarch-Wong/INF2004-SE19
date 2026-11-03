# Handset integration: connect, push-to-talk and disconnect over encrypted ESP-NOW

The first integration of the handset parts on one firmware. It works like a
phone call with a walkie-talkie inside:

1. **Connect.** Pick `Connect Board 2` on the menu. Board 2 shows **"Board
   1 wants to connect"** and beeps, and its user accepts or declines.
2. **Talk.** While connected, hold OK to talk and release to listen (half
   duplex). The other side sees `Board 1 is talking` and hears a short
   "roger" beep when you let go.
3. **Disconnect.** CANCEL on either board ends the connection **on both**.
   If the link drops (a board is switched off, or walks out of range), both
   sides drop the connection by themselves, usually within about 2 s and
   at most 6 s.

- **Voice:** the MAX9814 mic and PWM speaker chain from
  [mic_speaker_e2e_test2](../mic_speaker_e2e_test2/), with Opus,
  end-to-end AES-GCM and NACK retransmission over ESP-NOW.
- **UI:** the six buttons and the ST7789 LCD from
  [dpad_menu_test](../dpad_menu_test/).

The handsets are **pre-paired for up to five boards**. The shared key file
[`../shared_keys/e2e_keys.h`](../shared_keys/README.md) holds one pair key
for every pair of boards (10 keys).
Each board's firmware compiles in only the 4 it shares with the others, so
there is no pairing step. Boards 1 and 2 keep the network key and 1–2 pair
key of `mic_speaker_e2e_test`. Boards 1–3 have MACs; slots 4 and 5 are
ready but wait for MACs (see [Adding a board](#adding-a-board)). Boards 1
and 2 have the LCD, D-pad, mic and speaker wired. LoRa comes next and is
not in this build.

Status: **all five environments build, and the host tests pass (including
the connection and key-file checks). Not yet run on hardware.**

## Wiring

Every board runs the same firmware and uses whatever is wired to it. Pins
follow the Week 6 design review and the D-pad tests.

| Part | Pin | ESP32-S3 |
| --- | --- | --- |
| MAX9814 mic | VDD / GND / OUT / GAIN | 3V3 / GND / **GPIO1** (ADC1 ch 0) / 3V3 |
| Speaker module | VCC / GND / SIG | 5V / GND / **GPIO8** (8-bit PWM) |
| ST7789 LCD | VCC / GND | 5V / GND |
| | SCL / SDA | GPIO12 / GPIO11 (SPI, no MISO, no CS) |
| | RES / DC / BLK | GPIO10 / GPIO9 / GPIO14 |
| Button UP | 3V3 → | GPIO48 |
| Button DOWN | 3V3 → | GPIO47 |
| Button LEFT | 3V3 → | GPIO21 |
| Button RIGHT | 3V3 → | GPIO45 |
| Button OK (confirm) | 3V3 → | GPIO20 |
| Button CANCEL | 3V3 → | GPIO0 |
| Button PTT (optional) | 3V3 → | not set (`ptt_pin = -1`) |

Each button connects **3V3** to its GPIO (normally open). The internal
pull-downs are on, so pressed reads HIGH. Never connect a button to 5 V.
These pins stay clear of the LoRa pins in the design review (GPIO5, 6, 7,
15, 16, 17, 18), so LoRa can be added without moving anything.

Side effects, as in `dpad_test`:

- **GPIO20 (OK)** is native USB D+. Use the USB-to-UART connector only.
- **GPIO45 (RIGHT)** is a strapping pin. Holding RIGHT at reset stops the
  board from booting.
- **GPIO0 (CANCEL)** is the BOOT pin. **BOOT is no longer push-to-talk.**
  Don't press BOOT while holding CANCEL: that shorts 3V3 to GND. If the boot
  log shows `CANCEL ... (held at startup, ignored)` with nothing pressed,
  the board's GPIO0 pull-up is winning. Rewire CANCEL from GPIO0 to GND and
  set `cancel_active_low = true` in `include/app_config.h`.

The design review asks for a **dedicated PTT button**. Until one is wired,
holding **OK** while connected is push-to-talk. To add one, wire it like the
others and set `ptt_pin` to a free GPIO (for example 13 or 2) in
`include/app_config.h`.

## Screens and buttons

```
            OK on "Connect Board 2"           other side accepts
   Menu ─────────────────────────▶ Connecting ───────────────────▶ Connected ◀─┐
    ▲                                  │ CANCEL / declined /         │          │
    │                                  │ no answer (30 s)            │ CANCEL on either side,
    │   3 s or any button              ▼                             │ or link lost (6 s)
    └──────────────────────────── Disconnected ◀────────────────────┘          │
                                       ▲                                        │
              "Board 1 wants to connect" (appears on any screen) ── OK ─────────┘
                                       └── CANCEL: "You declined"
```

| Screen | Shows | Buttons |
| --- | --- | --- |
| Menu | `Connect Board N` for each other provisioned board (green dot = online now, see Presence), `Volume`, `Status`; scrolls when there are more than 4 rows | UP/LEFT, DOWN/RIGHT choose; OK selects |
| Connecting | Asking Board 2, time waiting | CANCEL stops asking |
| Request | **Who wants to connect**, "verified by pair key"; the speaker beeps every 1.5 s | OK accepts, CANCEL declines |
| Connected | Peer name, connection time, talk box, volume, last heard | Hold OK (or PTT) to talk; UP/RIGHT, DOWN/LEFT volume; **CANCEL disconnects** |
| Disconnected | Peer and reason | Any button, or 3 s, back to the menu |
| Volume | 1–32 with a bar | UP/RIGHT louder, DOWN/LEFT quieter, CANCEL back |
| Status | ID, MAC, channel, online/offline and session per handset, frame counters, uptime | CANCEL back |

The talk box on the Connected screen shows:

- `Hold OK to talk` when idle
- **`TALKING`** (red) while this board sends
- **`Board 1 is talking`** (green) while the other side's voice plays
- `Can't talk` (orange) if push-to-talk is refused

You can't talk while the other side is talking (half duplex). The green box
already shows who is talking.

If both sides press OK at the same moment, neither has heard the other
yet, so both start sending. Each side drops the other's voice while it
sends. Without a tie-break, both would stay on `TALKING` with nobody
hearing anything. Instead, when either side hears the other's voice while
it is sending, both apply the same rule (`voice_link::yields_floor`): the
stream with the higher random ID keeps talking, and the other side stops
at once. The side that stops switches to `Board N is talking` and plays
the other side from its first word, because the missing first frames are
asked for again. Its log shows `TX_YIELD`. To talk, wait for the roger
beep and press OK again.

Reasons on the Disconnected screen:

| Shown | Meaning |
| --- | --- |
| You disconnected | CANCEL pressed here |
| They disconnected | CANCEL pressed on the other board |
| Connection lost | The other board's radio stopped acknowledging (about 2 s), nothing came from it for 6 s, or it rebooted |
| Not reachable | The board you were asking went offline or rebooted |
| Declined / You declined | The request was declined |
| No answer | Nobody accepted within 30 s |
| Missed request | The other board stopped asking before you answered |
| Stopped asking | CANCEL pressed here while connecting |
| Busy | The other board is connected to someone else, or being asked |

This board's own name is at the top right of every screen. Names come from
`include/network_config.h` (`"Board 1"`, `"Board 2"`). Tones are set in
`include/app_config.h`: `ring_tone_*`, `roger_tone_*` and `refused_tone_*`.
0 ms turns a tone off.

## How connect and disconnect work

Connection messages are a fourth payload kind inside the voice channel
(`include/call.h`):

| Message | Sent by | When |
| --- | --- | --- |
| request | asking side | on Connect, then every 500 ms until answered |
| accept | asked side | on OK; repeated if a request arrives again |
| decline | asked side | on CANCEL |
| busy | asked side | when already connected or being asked |
| hang_up | either | on CANCEL (disconnect or stop asking), on giving up after 30 s, or on link loss |
| ping | both | every 1 s while connected (heartbeat) |

- **Disconnect reaches both sides.** The side that presses CANCEL drops at
  once and sends hang_up. If that message is lost, the other side's next
  heartbeat for the closed connection gets hang_up again.
- **Link loss.** The other board's radio acknowledges every frame it
  receives (ESP-NOW unicast). If 3 frames in a row fail, with none
  acknowledged for 2 s (`ack_lost_*`), the connection ends as `Connection
  lost` and the log shows `stopped acknowledging (radio)`. Heartbeats and
  presence pings keep frames going even when nobody talks. As a backstop,
  each side also drops the connection after 6 s with nothing from the
  other; voice frames count as well as heartbeats. If only one direction
  fails, the side that notices first drops, and its hang_up takes the
  other side down if it gets through.
- **Leftover streams.** When a new connection starts, anything still
  playing or draining from the last one is dropped (`RX cut`), so the new
  connection can talk at once.
- **Reboots.** A rebooted board makes a new session at boot. The other
  side sees the session change and ends whatever it had with that board:
  a connection shows `Connection lost` and a request shows `Not
  reachable`, about a second after the rebooted board is back up (or
  after 6 s offline, if the reboot takes longer). An old request never
  rings on the rebooted board.
- **Both connect at once:** the lower ID keeps asking, and the higher ID
  shows the request.

### Presence

A session is made once at boot and never expires, so it can't tell you
whether the other board is still on. Every board therefore sends a
one-byte presence ping, sealed under the session key, to each handset it
has a session with every 2 s (`presence_ping_ms`). A handset counts as
online while anything from it has opened in the last 6.5 s
(`presence_timeout_ms`, three missed pings). Pings, heartbeats and voice
all count, and so does a fresh handshake.

- The menu dot, the Status page and `Connect` follow this. Connecting to
  an offline board is refused at once with `not reachable` instead of
  waiting 30 s for `No answer`.
- The log shows `PEER N (Board N) online` / `offline` on each change, and
  the 10 s status line marks stale sessions `(offline)`.
- A board that comes back goes green quickly: after a reboot within
  about a second, through the new handshake; after being out of range
  within about 5 s, through the next ping.
- A request or connection in progress ends when its board goes offline
  or makes a new session (`check_call_peer`, log `LINK N ... ending`). So
  a request sent just before the other board went off shows `Not
  reachable` within 6.5 s. It isn't resent until the board comes back and
  then rings there with nobody expecting it.

`src/call.cpp` has no radio or timing code. `tests/call_test.cpp` runs it
over a simulated link with:

- lost accepts, declines and hang-ups
- one-way loss
- timeouts
- glare
- a busy third board
- reboots
- `millis()` wrapping

Deliberately breaking the repeat-the-answer rule, the glare rule, the
re-accept, or the link-loss hang_up each makes a test fail.

**Why the name on the request can be trusted.** Every connection message
is sealed with AES-GCM under the session key, like voice. That key comes
from the pre-shared pair key in a fresh handshake at every boot. A request
opens only if the other holder of the 1–2 pair key made it, so "Board 1
wants to connect" is authenticated. Holding only the network key isn't
enough. The session's replay window rejects replays, and old recordings
fail after any reboot.

Voice is gated by the connection. `talk` and `test` are refused unless
connected. A new incoming stream plays only from the connected handset;
others are counted as `not_connected` in `report`.

## Build and host tests

```sh
pio run                          # board1 .. board5
sh tests/run_host_tests.sh       # crypto, session
sh tests/run_handset_tests.sh    # connections, voice link, audio DSP, check_streams, key file
```

Validated 2026-11-03: all five environments build with no warnings, and
all host suites pass. The screens have no host tests; H1–H9 below cover them.

## Keys: pre-pairing for five boards

The keys live in [`tested_parts/shared_keys`](../shared_keys/README.md), one
file shared with the other projects that run on these boards. A board has
the same keys whichever project it runs. `platformio.ini` adds
`../shared_keys` to the include path.

| Key | Held by | Purpose |
| --- | --- | --- |
| Network | every board | Hop tag on every frame |
| Pair `a-b` (10 of them: 1-2 … 4-5) | boards a and b only | Session handshake with that one board; the session key seals voice and connection messages |

```sh
python3 ../shared_keys/generate_keys.py --check   # verify: all 10 pairs, both sides match, all distinct
python3 ../shared_keys/find_keys.py .pio/build/board*/firmware.bin
```

Checked 2026-11-03: `find_keys.py` finds the network key plus exactly 4 pair
keys in each `firmware.bin`. Board 1 holds 1-2, 1-3, 1-4 and 1-5, and so
on; no board holds a pair it isn't part of. A lost board exposes its own
pairs, not the others' conversations. It does expose the shared network
key, as in stages 1–3.

The file is **committed on purpose**; anyone with this repository can read
the voice. For a private deployment see
[shared_keys/README.md](../shared_keys/README.md#security).

## Adding a board

Slots live in `include/network_config.h`. Boards 1–3 have MACs. Board 3 is
the third board from the e2e tests; confirm it before use. Boards 4 and 5
are all-zero, which means not provisioned: no board can use that ID, the
others leave it off their menus, and its frames are dropped.

1. Flash `board4` (or any build) on the new board. It stops at boot with
   `This MAC is not in network_config.h`, and its screen shows **Stopped**
   with the board's MAC. The serial log shows `Wi-Fi station MAC: ...`, and
   `esptool read_mac` works too.
2. Put that MAC in slot 4, and change the name if you like (at most 12
   characters).
3. Flash `board4` on the new board, and rebuild and reflash the others, so
   their menus list it and their radios accept its frames.

The keys are already in place, so no key step is needed.

## Flash

**The user flashes the boards.** Before each upload, hold BOOT, tap RESET,
then release BOOT. Press RESET after the upload. Release all D-pad
buttons, especially RIGHT, while resetting.

```sh
pio run -e board1 -t upload --upload-port /dev/cu.usbmodem5B5F0216001
pio run -e board2 -t upload --upload-port /dev/cu.usbmodem5B5F0215971
pio run -e board3 -t upload --upload-port <board 3 port>
```

Each board must get the environment matching its MAC slot. A wrong one stops
at boot with `Wrong boardN environment for this board`.

Serial runs at 921,600 baud.

## Hardware tests

| # | Test | Pass |
| --- | --- | --- |
| H1 | Boot both boards | Menu with this board's name at top right; the dot next to `Connect Board N` turns green once `SESSION established` (log `PEER N ... online`); the boot log lists every `BUTTON` with nothing `held at startup` |
| H2 | Every button on Menu, Volume and Status | Each press logs `BUTTON X` once and the screen follows; `UI screen=... draw_ms` under 400 ms |
| H3 | Connect: board 1 → `Connect Board 2`, OK on board 2 | Board 2 shows `Board 1 wants to connect` and beeps; both show Connected; both logs show `LINK connected` with the same id and session |
| H4 | Talk both ways: hold OK on board 1 and speak, release; then board 2 | Sender TALKING, receiver `Board N is talking`, speech heard, roger beep, `RX_END ... intact=yes` |
| H5 | Disconnect: CANCEL on board 2 | Board 2 `You disconnected`, board 1 `They disconnected` within a second; both back to the menu |
| H6 | Link loss: connect, then unplug board 2 | Board 1 shows `Connection lost` within about 3 s (log `stopped acknowledging (radio)`); after plugging back in and booting, board 2 is on its menu and can connect again |
| H7 | Decline, stop asking, no answer | CANCEL on the request → board 1 `Declined`; CANCEL while connecting → board 2 `Missed request`; nobody answers → `No answer` after 30 s |
| H8 | Known signal: connected, `test 30` on board 1, then on board 2 | `tools/check_streams.py` passes, as E1 in `mic_speaker_e2e_test` |
| H9 | Alternating PTT for two minutes (design review) | No reset, no disconnect; every `RX_END` `intact=yes`; no sustained underruns in `report` |
| H10 | New board: flash `board4` on a board whose MAC is not in slot 4 | Stops with `This MAC is not in network_config.h` and its MAC on screen; after adding the MAC and reflashing, it boots, and the others (rebuilt) list `Connect Board 4` |
| H11 | Both press OK together, several times | Within a fraction of a second one board shows `Board N is talking` and logs `TX_YIELD`, while the other stays on `TALKING` and is heard from its first word. Either board can win. `report` counts `yields` |
| H12 | Presence: on the menu, unplug board 2; plug it back in | Board 1's dot for Board 2 goes grey and logs `PEER 2 (Board 2) offline` within about 7 s; `Connect Board 2` then shows `not reachable` at once. After board 2 boots, the dot is green again within a few seconds and connecting works |
| H13 | Request to a board that goes off: board 1 → `Connect Board 2`, then unplug board 2 before answering; plug it back in | Board 1 shows `Not reachable` within about 7 s (or about a second after board 2 is back, if sooner), logging `LINK 2 ... ending connecting`. Board 2 boots to its menu and does not ring |
| H14 | Request guard: board 1 asks board 2 while board 2's user is pressing OK on its menu | Presses within 0.5 s of the request appearing log `BUTTON ignored: the request has just appeared` and do not accept. OK after that accepts |
| H15 | Board 3 off (it is listed in slot 3) | Boards 1 and 2 log 10 `SESSION hello -> 3`, then `SESSION 3 not answering: retrying every 15 s` and go quiet. H4 and H9 voice is unaffected |
| H16 | Reconnect at once: connect, hold OK on board 1 and speak, press CANCEL on board 2 mid-sentence, connect again right away | The new connection can talk at once; any leftover logs `RX cut` and no roger beep |

H8 from serial with `tools/capture_logs.py`:

```sh
~/.platformio/penv/bin/python tools/capture_logs.py h8 \
    b1=/dev/cu.usbmodem5B5F0216001 b2=/dev/cu.usbmodem5B5F0215971@1 \
    --duration 70 --send "b1@8=connect 2" --send b2@10=accept \
    --send "b1@15=test 30" --send b1@60=report --send b2@60=report
python3 tools/check_streams.py evidence/h8_b1.txt evidence/h8_b2.txt
```

## Commands

| Command | Does |
| --- | --- |
| `connect N` | Ask handset N to connect (as `Connect Board N` on the menu) |
| `accept` / `decline` | Answer a request |
| `disconnect` | Disconnect, or stop asking |
| `link` | Connection state, peer, id |
| `talk [S]` / `test [S]` / `stop` | Talk, or send the test signal, from serial (connected only) |
| `buffer MS`, `red on\|off` | Playout buffer, redundancy |
| `+` / `-` / `vol N`, `]` / `[` / `gate R`, `m`, `r`, `gain X` | Audio, as in `mic_speaker_e2e_test` |
| `mic`, `micdump S`, `tone HZ [MS]`, `sweep` | Mic and speaker checks |
| `report`, `radio CH MBPS`, `keys`, `reboot` | Counters, radio, keys, restart |

The audio chain, gate strictness and "no sound" checks are as in
[mic_speaker_e2e_test](../mic_speaker_e2e_test/README.md).

## Limits

Open issues and planned fixes, mostly for more than two boards, are
tracked in [ISSUES.md](ISSUES.md).

- **Not run on hardware yet.** The D-pad tests are also still waiting on
  hardware validation, including whether GPIO20 and GPIO0 work as buttons
  on these boards.
- **Half duplex.** One side talks at a time. Full duplex would need echo
  cancellation.
- **Link loss takes about 2 s to notice** (radio acknowledgements), up to
  6 s if acknowledgements still get through but nothing else does.
- **Offline takes up to 6.5 s to show on the menu.** A board that just
  switched off stays green until its pings stop or three of ours go
  unacknowledged.
- **Redraw time.** A full-screen redraw at 10 MHz SPI blocks the loop for
  about 100 ms. It happens only when the screen changes, and during a
  connection only the box and small fields redraw. Voice has a 300 ms
  playout buffer and a 320 ms send queue. Check `draw_ms` and `intact` in
  H4. Raise `lcd_spi_hz` once the wiring is known to be good.
- **One connection at a time per board.** With five boards, 1–2 and 3–4
  can be connected at once, but no group calls. A third board asking a
  connected board gets `Busy`.
- **No "All" (broadcast) mode yet.** When added, it will be **unencrypted**
  (the user's decision), like `opus_broadcast_test`. Anyone on the channel
  can hear it, and it must be kept apart on receive, so a plaintext frame
  is never accepted as a connection's voice.
- **Absent boards are tried slowly.** The radio retries every frame to an
  absent board, and voice waits behind it. So a board sends 10 session
  hellos a second apart to a board that is off, then one every 15 s
  (`SESSION 3 not answering: retrying every 15 s`). Presence pings to an
  offline board go every 5 s instead of 2 s. A rebooted board still
  connects within about a second, because it sends hellos at boot and the
  lower ID answers at once. A board that was never seen and comes into
  range can take up to 15 s to show.
- **Boards 3–5 untested.** Only boards 1 and 2 have the handset parts wired.
  If board 3 runs other firmware on channel 11, its frames now pass the MAC
  allowlist and show up as `REJECT` lines.
- **Keys are public** (see Keys).

## Project files

- `include/call.h`, `src/call.cpp`: connection messages and state machine.
- `src/ui.inc`: D-pad, screens and navigation (from `dpad_menu_test`).
- `src/handset.inc`: voice send/receive from `mic_speaker_e2e_test2`,
  plus connection handling. Voice is gated on a connection, BOOT is no
  longer push-to-talk, and a roger beep plays at the end of each received
  stream.
- `src/main.cpp`: keys, sessions, receive dispatch, serial commands.
- `include/app_config.h`: pins (mic, speaker, LCD, buttons), audio, UI and
  connection timings, tones.
- `include/network_config.h`: five board slots: IDs, MACs (zero = not
  provisioned) and names.
- `../shared_keys/`: the network key and 10 pair keys, shared with other
  projects, plus the generator and key finder.
- `tests/call_test.cpp`: connection host tests. The others are as in
  `mic_speaker_e2e_test2`.
