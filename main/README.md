# Main project: handset ESP-NOW flood build

The team's main firmware. It is
[tested_parts/handset_espnow_flood_integration_test](../tested_parts/handset_espnow_flood_integration_test/)
as pushed in a59574f, copied to the repository root. Its keys are in
[shared_keys/](shared_keys/), a copy of
[tested_parts/shared_keys](../tested_parts/shared_keys/) with the same keys,
so `main/` builds on its own.

That build is a copy of [handset_broadcast_integration_test](../tested_parts/handset_broadcast_integration_test/)
cut down to **the ESP-NOW flood mesh, LoRa, Broadcast and LoRa broadcast**,
plus Status and Tools. The changes to how the flood mesh relays are below.

| Menu row | What it does | Reaches a board out of range |
| --- | --- | --- |
| **ESP-NOW flood** | Pick a board, it accepts; hold OK to talk (Opus), RIGHT hello, UP/DOWN volume | yes: every board that isn't connected itself relays |
| **LoRa** | Pick a board, it accepts; UP "Hello from Board N", DOWN "Bye from Board N", with receipts | no |
| **Broadcast** | No connection: hold OK to talk to all, RIGHT "Hello all" | no: not relayed |
| **LoRa broadcast** | No connection: UP "Hello all", DOWN "Bye all" | no |
| **Status** | IDs, sessions, who is online over the flood mesh, LoRa radio, counters | |
| **Tools** | `Block Board N`, ESP-NOW rate, `Mute flood`, `Mute LoRa` | |

**Status: all six environments build, host tests pass (2026-11-24). Not
tested on hardware: the flood mesh never has been, in any project.**

New here? Follow the [guide](#guide-from-parts-to-a-first-call) below: parts,
wiring, flashing and a first call. The sections after it explain how the
flood mesh works and how to test it.

## Guide: from parts to a first call

### What you need

- Two or more ESP32-S3 N16R8 boards, up to six. A third board placed
  between two others shows the relaying.
- Per handset: a MAX9814 microphone, a speaker module, an ST7789 240 × 240
  LCD, six push buttons and a Waveshare Core1121 LoRa module with a 915 MHz
  antenna.
- A USB cable to each board's **UART** connector (not the native USB one:
  the OK button uses a USB data pin).
- [PlatformIO](https://platformio.org/install) (VS Code extension or the
  command line).

Every board runs the same firmware and uses whatever is wired to it. A
board used only as a relay needs none of the parts, only power; it can be
driven over serial.

### Wiring

Wire with the board unplugged. Pins are set in `include/app_config.h`.

| Part | Pin | ESP32-S3 |
| --- | --- | --- |
| MAX9814 mic | VDD / GND / OUT / GAIN | 3V3 / GND / **GPIO1** / 3V3 (AR not connected) |
| Speaker module | VCC / GND / SIG | 5V / GND / **GPIO8** |
| ST7789 LCD | VCC / GND | 5V / GND |
| | SCL / SDA | GPIO12 / GPIO11 (no MISO, no CS) |
| | RES / DC / BLK | GPIO10 / GPIO9 / GPIO14 |
| Core1121 LoRa | MISO / MOSI / SCK | GPIO5 / GPIO6 / GPIO7 |
| | NSS (CS) / RESET | GPIO15 / GPIO16 |
| | BUSY / DIO9 (IRQ) | GPIO17 / GPIO18 |
| | VCC / GND | 3V3 / GND |
| Button UP | 3V3 → | GPIO48 |
| Button DOWN | 3V3 → | GPIO47 |
| Button LEFT | 3V3 → | GPIO21 |
| Button RIGHT | 3V3 → | GPIO45 |
| Button OK | 3V3 → | GPIO20 |
| Button CANCEL | 3V3 → | GPIO0 |

- Each button connects **3V3** to its GPIO; the internal pull-downs are on.
  Never connect a button to 5 V.
- Attach the LoRa antenna before powering the board.
- **GPIO45 (RIGHT)** is a strapping pin: holding RIGHT at reset stops the
  board from booting.
- **GPIO0 (CANCEL)** is the BOOT pin. Don't press BOOT while holding CANCEL
  (that shorts 3V3 to GND). If the boot log shows `CANCEL ... (held at
  startup, ignored)` with nothing pressed, wire CANCEL from GPIO0 to GND
  instead and set `cancel_active_low = true`.
- There is no separate push-to-talk button (`ptt_pin = -1`): holding OK
  while connected is push-to-talk.

### Boards

Each board is built for its own ID, and checks at boot that its MAC matches
(`include/network_config.h`).

| Board | Environment | Wi-Fi station MAC |
| --- | --- | --- |
| 1 | `board1` | `E0:72:A1:D8:E2:10` |
| 2 | `board2` | `E0:72:A1:D7:FA:B8` |
| 3 | `board3` | `E0:72:A1:D8:F6:20` |
| 4 | `board4` | `E0:72:A1:D8:0E:48` |
| 5 | `board5` | `E0:72:A1:D5:64:0C` |
| 6 | `board6` | `E0:72:A1:D9:A4:94` (added 2026-11-24 as an extra relay) |

The table is the one from `handset_peer_mesh_integration_test`, not the
copied project's, plus board 6. To use a different board, flash any
environment on it, read `Wi-Fi station MAC` from its boot log, put that MAC
in the slot you want, then rebuild and flash **every** board so they all
list it. The keys need no step: `shared_keys/e2e_keys.h` already holds a
pair key for every pair of the six boards.

### Build and flash

Every board must run this build: the flood envelope is the same on the
wire, but a board on an older build tells copies apart the old way.

```sh
cd main
pio run                      # all six boards
pio run -e board1 -t upload  # then board2, ... one board per environment
pio device monitor           # 921600 baud
```

Before each upload hold **BOOT**, tap **RESET**, release **BOOT**; tap
**RESET** again afterwards. With several boards plugged in, add
`--upload-port` and `--port`.

### A first call

1. Power two flashed boards. Each shows the menu: `ESP-NOW flood`, `LoRa`,
   `Broadcast`, `LoRa broadcast`, `Status`, `Tools`. UP/DOWN choose, OK
   selects, CANCEL goes back.
2. On board 1 open **ESP-NOW flood** and press OK on `Connect Board 2`. A
   dot beside a board means it is online over the flood mesh.
3. Board 2 beeps and shows the request. **OK** accepts, **CANCEL**
   declines.
4. Once connected, **hold OK to talk** and release to listen (one side at a
   time). **RIGHT** sends "Hello from Board N", **UP/DOWN** set the volume,
   **CANCEL** disconnects both sides.
5. **See a relay.** Power a third board with this build. On board 1 choose
   Tools → `Block Board 2`, and on board 2 `Block Board 1`, so the two
   can't hear each other directly. Connect again: the connected screen
   shows the path, for example `Route: 1 > 3 > 2, via Board 3`.
6. **LoRa text.** Menu → **LoRa** → a board, which accepts. UP sends "Hello
   from Board N" and DOWN "Bye from Board N"; each shows `Delivered` or
   `Not delivered`.
7. **Broadcast.** Menu → **Broadcast** needs no connection: hold OK to talk
   to every board in range, RIGHT sends "Hello all". The boards that hear
   the voice jump to their Broadcast page, which names the talking board as
   `Host`. When the host presses CANCEL they all go back to the page they
   were on ([Leaving a broadcast together](#leaving-a-broadcast-together)).
   **LoRa broadcast** sends "Hello all" (UP) and "Bye all" (DOWN).

Everything on the buttons can also be done from the serial monitor; see
[Serial commands](#serial-commands-optional).

### If something goes wrong

| What you see | Cause and fix |
| --- | --- |
| Upload can't connect | Hold BOOT, tap RESET, release BOOT, then upload again |
| Nothing on the serial monitor | Use the UART connector at 921600 baud |
| Board doesn't start | RIGHT was held at reset (GPIO45), or BOOT is still held |
| `Built for device N, but this board is device M` | The wrong environment is on this board, or its MAC is not in `include/network_config.h` |
| A board never shows as online | It is off, on another build or channel, or reachable only through a board that is connected to another (a connected board doesn't relay) |
| LoRa row shows a failed radio | Check the Core1121 wiring and antenna; the radio is retried every 5 s and the rest of the handset keeps working |
| No sound | Raise the volume with UP on the connected screen; `m` on serial toggles mute |

## What was cut

| Gone from the menu | Still there? |
| --- | --- |
| ESP-NOW (direct connection) | Serial `connect N` still makes one. The pair sessions under it stay: they carry the broadcast keys |
| ESP-MESH | Removed from the source (`mesh.inc`, `mesh_radio.cpp`): no mesh on the air, no `mesh` commands |
| Routed relay | Removed from the source; a routed frame from another build is rejected like any unknown frame |
| Volume page | UP/DOWN on the connected and Broadcast screens still set the volume. Mute is serial `m` only |
| Routes page | The route shows on the connected screen (`Route: 1 > 3 > 2, via Board 3`) |
| Tools: leave/rejoin mesh, be mesh root, mute mesh, mute routed | |

The menu is `load_rows()` (`src/ui.inc`). The unused screens (ESP-NOW
list, ESP-MESH, Routed relay, Volume, Routes) and transports (ESP-MESH,
the routed relay) were deleted from the source on 2026-11-24;
`tested_parts/handset_broadcast_integration_test` still has them.

## How the flood mesh works

1. Board 1 seals a frame for board 2 under their flood session key, puts
   it in an **envelope** and sends it as an ESP-NOW **broadcast**. The
   envelope holds a hop limit (4 transmissions, `relay_max_hops`) and the
   path so far (`1`).
2. Every board that hears it checks the frame's **hop tag** (HMAC under the
   network key), drops it if it has seen that frame in the last 5 s or is
   already on its path, and otherwise broadcasts it again with itself
   added to the path and one hop fewer. A connected board (over either
   radio) doesn't pass frames on, presence pings included, so boards that
   reach each other only through it show as offline until it disconnects.
   A board that is only asking or being asked still relays.
3. Board 2 takes the first copy, opens it under the session key, and drops
   the later copies.

Every path is used at once, so there is nothing to set up or repair. The
cost is airtime: each frame is sent once by every board that hears it.
Broadcasts get no radio acknowledgement; the voice link's redundancy (each
frame carries the one before) and NACKs cover lost frames. Sessions,
presence pings, connection messages, voice and hello all travel this way.

## What changed in the flood mesh

| | Before | Now |
| --- | --- | --- |
| Telling a copy from a new frame | By the envelope's ID, which is not authenticated | By the frame's hop tag, already checked with the network key |
| When a board relays | At once | After a random 0–20 ms (`relay_jitter_ms`) |
| Which boards relay | Every board without a connection, a request counting as one | Every board that isn't connected; one asking or being asked still relays |
| Asking again for a missing voice frame | After 40 ms, as over direct ESP-NOW | After 100 ms (`relay_nack_retry_us`) |
| Voice packets | 50 a second, each with the frame before it | The same, or 25 a second with `flood pair on` |

**Copies.** The envelope's ID counts up in the clear and nothing signs it.
Anyone with a radio could replay one old frame under the ID of a frame not
sent yet; every board that heard it would then drop the real frame as a
copy. A stream could be silenced that way without any key. The duplicate
filter now remembers the first four bytes of the hop tag, which differ for
every frame and can't be made up without the network key. A replayed frame
is the copy, and the real one goes through
(`tests/relay_frame_test.cpp`). The ID stays in the envelope for logs and
sniffers; nothing trusts it.

**Timing.** Every board that hears a frame hears it at the same moment.
Relaying at once, two boards that can't hear each other but share a
neighbour transmit on top of each other for every frame, and that
neighbour gets neither copy. With a random wait they overlap for about one
voice frame in six (a frame is about 1.7 ms on air at 1 Mbps), which
redundancy and NACKs cover. It adds 10 ms a hop on average. Boards that do
hear each other already took turns (Wi-Fi's carrier sense). Set
`relay_jitter_ms` to 0 in `include/app_config.h` to relay at once as
before.

**Which boards relay.** A request waits up to 30 s for an answer, and a
connection is given up as lost after 6 s of silence. While a request
counted as a connection, asking a board that was relaying for two others
ended their connection before anyone had answered. Now only a connected
board stops relaying.

**Asking again.** A board asks again for a missing voice frame (NACK) if
the frame hasn't come. Each relay holds the NACK and the frame sent again
for up to 20 ms, so across two relays the answer can take 80 ms. Asking
again after 40 ms, every board flooded a second NACK and a second copy of
the frame, just when frames were being lost. 100 ms leaves three tries
inside the 300 ms playout buffer.

**Voice packets.** Airtime is what limits a flood mesh: with six boards in
range a talker's 50 packets a second become 250 on the channel, about 40%
of it at 1 Mbps. `flood pair on` (serial, on the board that talks) sends
every other frame with the one before it, as the ESP-MESH builds do: 25
packets a second of the same size, so half the airtime. Each frame then
goes once, so a lost packet costs two frames and only a NACK brings them
back. It is off by default (`flood_pair_default`) and lasts until the next
reset; try it if voice breaks up with many boards in range. A faster
ESP-NOW rate is not the fix: broadcasts lost 10–27% of frames at 6 Mbps
and almost none at 1 Mbps (`TRACKING.md` A1), and every flood frame is a
broadcast.

Considered and left out: a board cancelling its relay once it hears
another board relay the same frame. It halves the airtime when every board
hears every other, but a board that is the only way to the destination
would also cancel, and frames would go missing exactly when a relay is
needed.

## Test cases

None of these needs a board; nothing is uploaded.

```sh
sh tests/run_all_tests.sh        # all of them, about 2 minutes
sh tests/run_build_tests.sh      # T1-T4; add board1 ... to build fewer
sh tests/run_host_tests.sh       # T7, T9, T10
sh tests/run_handset_tests.sh    # T5, T6, T8
```

| ID | Test case | Passes when |
| --- | --- | --- |
| T1 | Every board's firmware compiles (`board1`–`board6`) | `pio run -e boardN` succeeds and `firmware.bin` is made |
| T2 | No compiler warnings from the project | No `warning` line names a file in `src/` or `include/` |
| T3 | Each board gets its own firmware | No two `firmware.bin` files are the same |
| T4 | The PC and test scripts are valid | `tools/*.py` compile, `tests/*.sh` parse |
| T5 | The menu is cut down (`tests/cutdown_test.py`, reads the source) | Menu rows are ESP-NOW flood, LoRa, Broadcast, LoRa broadcast, Status, Tools; Tools has the rate, Mute flood, Mute LoRa; ESP-MESH and the routed relay are not in the source |
| T6 | The settings are sane (`tests/config_test.cpp`) | Board IDs in order, no MAC twice, names fit the screen, the largest voice frame fits a flood envelope, the wait before asking again for a voice frame covers two relays |
| T7 | Flood duplicate filter, hop limit and what a board does with a frame (`tests/relay_frame_test.cpp`) | A copy is dropped; a frame replayed under the next ID doesn't block the real one; a hop count raised on the way is refused; a frame is sent 4 times and no more; the destination takes it, another board passes it on, the origin and boards on its path drop it, a path that doesn't match is refused |
| T8 | The inherited host suites: crypto, sessions, connections, LoRa texts, voice link, audio DSP, key file | Each prints `All ... passed` |
| T9 | A hello replayed during a handshake (`tests/session_test.cpp`) | One replayed between the reply and the confirm doesn't stop the session, with the confirm or without it; three don't either; four do, and a new handshake recovers |
| T10 | A session is bound to its transport (`tests/session_test.cpp`) | A reply or confirm made for LoRa is refused on the flood mesh and the other way round; the same pair key and nonces give a different session key on each |

Last run 2026-11-24, with board 6 added: all pass (21 build checks, every
host suite).

Testing on the boards is a separate step, after flashing:
`tools/relay_test.py` and `tools/broadcast_test.py` drive boards 1 and 2
over serial (the docstring of each has the command). `tools/lora_edge_test.py`
is unchanged from the copied project and opens the ESP-NOW menu row, which
this build doesn't have, so its ESP-NOW and mixed cases won't run here.

## Serial commands (optional)

`help` lists them. For the flood mesh: `flood`, `flood connect N`,
`flood mute on|off`, `flood pair on|off`, `relay block N`,
`relay unblock N`. `accept`, `decline`, `disconnect`, `hello`, `talk`,
`test` and `stop` act on the connection in progress. The `flood` line
counts frames sent, relayed, dropped as copies and delivered, and
`busy drops`: frames for other boards this board didn't pass on because it
was connected itself.

Broadcast, LoRa and the log lines are as in
[handset_broadcast_integration_test](../tested_parts/handset_broadcast_integration_test/README.md#serial-commands-optional),
plus `bcast close`: what CANCEL on the Broadcast page sends, for a board
that talked with `bcast talk` or `bcast test` from another page. The
`bcast` status ends with the board this one follows.

## Leaving a broadcast together

A board brought to the Broadcast page by another board's voice follows
that board, its **host**. The follower leaves the page, back to the one it
was on, when:

| The host | The follower's banner | How it knows |
|---|---|---|
| presses CANCEL, or accepts a connection | `Board N ended broadcast` | A close frame, broadcast 4 times 60 ms apart after the last voice frame, sealed under the host's broadcast key |
| restarts | `Board N restarted` | The host's new broadcast key arrives |
| is switched off or out of range | `Board N is out of range` | No presence ping and no broadcast from it for 6.5 s |

- A board that opened the page from its own menu follows no one, so
  nobody's close moves it.
- A follower that presses CANCEL leaves alone. If it had talked back, the
  boards that only its voice brought to the page leave with it.
- A close is taken during a connection too, so a follower that took a
  call doesn't come back to an empty Broadcast page.
- A follower that misses all four copies stays until CANCEL; at the 27%
  loss measured for broadcasts that is 1 time in 190.
- Any board holding the host's broadcast key could send a close in its
  name ([Security](#security): a broadcast key is shared).

## Security

What protects a message, and what does not. The keys are demo keys in a
public repository (see [Limits](#limits)), so read this as what the design
does for a team that keeps its key file private.

| Protection | How |
| --- | --- |
| Only the two boards of a connection read it | AES-128-GCM under a session key. Each pair of boards has its own compiled-in pair key; no third board, relays included, holds it |
| A new key every boot | The session key is HKDF-SHA256 of the pair key and two fresh 16-byte random nonces, one from each board. Each way between two boards (direct ESP-NOW, flood, LoRa) has its own session |
| Changed or forged messages are refused | 16-byte GCM tag over the message and its header (type, source, destination, counter) |
| Replays are refused | The counter starts at 1 in each session and is never reused; the receiver keeps a 64-counter window, moved only by messages whose tag passed. A recording from an earlier session fails the tag |
| Only boards of the network are relayed | Every frame ends in a hop tag, HMAC-SHA256 under the network key, which a relay checks before passing it on. A relay can't open the message |
| Only the paired board can start a session | The handshake's reply and confirm carry HMAC tags under a key derived from the pair key, bound to both board IDs and both nonces. The responder keeps its working session until the new one is confirmed |
| A session belongs to one way between two boards | The way (direct ESP-NOW, flood, LoRa) is mixed into the handshake's tag key and the session key. A handshake picked up on one radio and passed on over the other fails its tag, and the same nonces give a different key on each |
| Broadcasts | Sealed the same way under a key the sender makes each boot and hands to each paired board inside their session |
| The primitives are the real ones | At boot the firmware checks AES-GCM, HMAC-SHA256, HKDF and X25519 against published test vectors and stops if one fails. Random numbers are taken after Wi-Fi starts, when the hardware generator has its noise source |

Someone with a radio and no keys can still:

- **See who talks to whom, when and how much.** The header and the frame
  length are in the clear. The content is not.
- **Jam the channel.** Nothing here prevents that, and the points below
  cost an attacker no less effort than jamming does.
- **Make idle boards relay an old frame again.** A hop tag never expires
  and the duplicate filter remembers 5 s, so a recorded frame sent again
  later is passed on once more by each idle board it reaches, within its
  4 transmissions. The destination refuses it as a replay. The cost is
  airtime.
- **Delay a handshake.** A hello proves nothing by itself, so an old one
  can be sent again. It can't disturb a working session. A board keeps the
  last four hellos it answered (`pending_slots`), so stalling a handshake
  takes four different recorded hellos sent between the real reply and
  its confirm. The two boards then start again after 5 s of failed frames
  (15 s over LoRa).

Accepted for a test project, and not defended:

- **No forward secrecy.** Whoever holds a pair key can work out every
  session key made from it, for traffic recorded before or after.
- **Keys sit in plain flash.** No flash encryption or secure boot: a lost
  board gives up the network key and its own five pair keys, not the
  other pairs.
- **A broadcast key is shared.** Every board given a sender's broadcast
  key reads its broadcasts and could send one in its name.
- **The flood envelope is not signed.** Its hop count and path change at
  every relay, so anyone can rewrite them. That can cut short how far a
  frame travels and change what the route line shows. It can't make the
  boards send a frame further: a relay refuses a hop count higher than the
  path allows (`hops_ok()`), so 4 transmissions stays the limit for each
  time a frame is put on the air. Someone who sends a recorded frame again
  with a new envelope starts another 4, as in the replay point above.

## Limits

- **Not run on hardware yet** as a flood mesh; the results above are builds
  and host tests.
- **Half duplex.** One side talks at a time.
- **One connection at a time per board**, over either radio. A connected
  board doesn't relay, so boards that reach each other only through it are
  cut off until it disconnects, and a connection relayed through it ends
  as link lost.
- **At most 4 transmissions per frame** (`relay_max_hops`): up to three
  boards relay it on the way.
- **Broadcasts are not relayed.** They reach only boards in direct range.
- **LoRa carries text only**, "Hello" and "Bye"; voice is ESP-NOW only.
- **Airtime.** Every board that hears a flood frame sends it once more, so
  more boards in range means more traffic on the channel: about 40% of it
  for one talker with six boards in range (an estimate, not measured).
  `flood pair on` halves it ([Voice packets](#what-changed-in-the-flood-mesh)).
- **Every board must run this build.** Its sessions are bound to their
  transport ([Security](#security)), so a board on this firmware and one
  on an earlier build, or on a `tested_parts` handset project, refuse each
  other's handshake (`bad handshake tag`) and show each other as waiting.
  Flash all six together.
- **The keys are demo keys, committed on purpose** (`shared_keys/`). Anyone
  with this repository can read the traffic; make new keys with
  `shared_keys/generate_keys.py --force` before any real use.

## What changed from handset_broadcast_integration_test

- `src/broadcast.inc`, `src/ui.inc`: a broadcast's listeners leave the
  Broadcast page when its host does
  ([Leaving a broadcast together](#leaving-a-broadcast-together)); the
  `bcast close` command.
- `src/ui.inc`: the menu and Tools rows listed above; the Status page
  without the ESP-MESH and routed lines; the flood row is labelled
  `ESP-NOW flood`.
- `src/main.cpp`: no ESP-MESH (`src/mesh.inc`, `src/mesh_radio.cpp` and
  `include/mesh_radio.h` are deleted); the `help` text.
- `src/relay.inc`: only the flood mesh, the routed relay is deleted (with
  its route messages in `relay_frame`); relays wait
  a random time (`relay_hold()`, `service_relay_queue()`); copies are told
  by the hop tag; a board no longer records its own frames in the
  duplicate filter (it already drops its own echoes by their source).
- `include/relay_frame.h`, `src/relay_frame.cpp`: `frame_key()`; the
  duplicate filter holds 32-bit keys.
- `include/session.h`, `src/session.cpp`: a board keeps the last four
  hellos it answered, not only the newest, so one replayed hello can't
  push out a handshake in progress ([Security](#security)).
- `include/session.h`, `src/session.cpp` and where each transport loads
  its keys (`src/main.cpp`, `src/relay.inc`, `src/lora.inc`): a session
  is bound to its transport
  (`session::transport`). The frames look the same, but the keys derived
  from a pair key are new, so **this build makes no session with the
  earlier builds**, the `tested_parts` handset projects included.
- `include/relay_frame.h`, `src/relay_frame.cpp`, `src/relay.inc`:
  `hops_ok()`. The hop count isn't signed, so a relay now refuses one
  raised on the way.
- `src/relay.inc`, `src/handset.inc`: only a connected board stops
  relaying (`any_connected()`); a request no longer counts.
- `include/relay_frame.h`, `src/relay_frame.cpp`: `classify()` and
  `add_hop()`, what a board does with a frame and how it passes one on,
  moved out of `src/relay.inc` so the host tests cover them.
- `src/handset.inc`: voice over the flood mesh asks again for a missing
  frame after `relay_nack_retry_us`; `flood pair on|off` (`flood_pair`).
- `include/app_config.h`: `relay_jitter_ms`, `relay_queue_depth`,
  `relay_nack_retry_us`, `flood_pair_default`, the project name.
- `include/network_config.h`: the board table of
  `handset_peer_mesh_integration_test`.
- `tests/relay_frame_test.cpp` (new), in `tests/run_host_tests.sh`;
  `tests/config_test.cpp` (new), in `tests/run_handset_tests.sh`.
- `tests/run_build_tests.sh` and `tests/run_all_tests.sh` (new).
- `tests/cutdown_test.py` (new), in `tests/run_handset_tests.sh`.
- `tools/relay_test.py`: flood only. `tools/mesh_test.py` removed.
