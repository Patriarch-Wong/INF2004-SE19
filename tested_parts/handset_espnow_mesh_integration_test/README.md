# Handset ESP-NOW mesh integration: flood mesh and routed relay

A copy of [handset_espmesh_integration_test](../handset_espmesh_integration_test/)
with the ESP-MESH functions duplicated **two more ways, both over ESP-NOW,
with every board relaying**:

1. **Flood mesh**: every board re-broadcasts every frame it hasn't seen
   before, a few hops at most.
2. **Routed relay**: a board finds a route first (route discovery), then
   each frame goes hop by hop along that route only.

**No router, no access point and no root.** ESP-NOW needs none of them, so
both are true mesh topologies: any board can relay for any other, with no
board in charge.

Everything from the ESP-MESH build is still here and unchanged in use:
ESP-NOW, ESP-MESH (with root failover) and LoRa.

| Function | ESP-NOW | ESP-MESH | **Flood mesh** | **Routed relay** |
| --- | --- | --- | --- | --- |
| Pick a board, ask to connect, accept/decline | yes | yes | yes | yes |
| Hold OK to talk (Opus), RIGHT hello, UP/DOWN volume, CANCEL | yes | yes | yes | yes |
| Green "online" dot (presence pings) | yes | yes | yes | yes |
| Route on the connected screen | direct | tree route | first copy's path | the route in use |
| Reaches a board out of range | no | through the tree | through any boards | through the route found |
| Topology | direct links | tree (one root) | **mesh** | **mesh** |
| Own session key per peer | yes | yes | yes | yes |

**One connection at a time over any radio**: while one is asking, asked or
connected, a request over another gets "Busy" and connecting over another
is refused.

**Status: all five environments build. Not yet tested on hardware.** The
host-tested modules (`call`, `voice_link`, `lora_text`, `audio_dsp`,
`command_parser`, crypto) are unchanged.

## How the two ways work

### Flood mesh

1. Board 1 sends a frame for board 2 as an ESP-NOW **broadcast**.
2. Every board that hears it and hasn't seen it before checks the
   network-key hop tag and broadcasts it once more. The hop limit is 4
   transmissions (`relay_max_hops`), and each relay adds itself to the
   path.
3. Board 2 takes the first copy. Every later copy is dropped by the
   duplicate filter: origin plus frame ID, remembered 5 s.

So **every path is used at once**: there's nothing to set up or repair,
and a board going down doesn't matter as long as another path exists. The
cost is that each frame is sent once by every board that hears it.
Broadcasts get no radio acknowledgement; the voice link's redundancy and
NACKs cover lost frames.

### Routed relay (route discovery, as AODV)

Board 1 wants board 2 and has no route to it:

1. **Ask:** board 1 broadcasts a *route request* "who reaches 2? (from 1,
   request #17)". This is the only flood.
2. **Breadcrumbs:** each board that hears request #17 for the first time
   notes the way back to board 1 and broadcasts it on, adding itself to
   the path.
3. **Answer:** board 2 receives the request and sends a *route reply* back
   along the request's path, unicast, carrying the whole route, e.g.
   `1 > 3 > 2`.
4. **Learn:** every board on the way learns routes to every board on that
   path. Board 1 now sends to board 2 via board 3.
5. **Send:** each frame goes hop by hop as **unicast**, acknowledged and
   retried by the radio at every hop, and only along the route. Boards
   off the route stay quiet.
6. **Repair:** if a hop stops acknowledging, routes through it are dropped
   and found again. A board that can't pass a frame on sends a *route
   error* back to the sender, which looks again. A route in use is looked
   for again every 20 s in the background (the old one carries on), so a
   shorter path replaces it when one appears.

| | Flood mesh | Routed relay |
| --- | --- | --- |
| Before the first frame | nothing | a route request and reply (tens of ms) |
| Who sends each frame | every board that hears it | only boards on the route |
| Radio acknowledgement per hop | no (broadcast) | yes (unicast) |
| A relay goes down | other paths carry on at once | ~2 s to notice, then a new route |
| Airtime | most | least |

Frame formats are in [include/relay_frame.h](include/relay_frame.h):
- **Envelope:** marker, TTL, ID and path, around the unchanged encrypted
  frame.
- **Route messages:** request, reply and error, signed with the network
  key.

The code is in [src/relay.inc](src/relay.inc): one implementation shared by
both ways.

### Security

- Every frame is sealed end to end exactly as over ESP-NOW (AES-GCM),
  under a **session key of its own per peer and per way**, made by the
  same handshake over that way.
- A relay checks the network-key hop tag before passing a frame on, and
  can't open it.
- Route messages carry an HMAC under the network key, so a board without
  the key can't forge routes.
- Frames from MACs not in `network_config.h` are dropped by the radio
  before any of this.

## What changed from handset_espmesh_integration_test

- `include/relay_frame.h`, `src/relay_frame.cpp` (new): the relay
  envelope, route messages and duplicate filter.
- `src/relay.inc` (new): both ways (sending, relaying, route discovery and
  repair) plus sessions, presence, connections, route lines and the
  serial commands.
- `src/handset.inc`: the shared voice and text code serves four Wi-Fi
  ways (`via::espnow`, `mesh`, `flood`, `routed`). A single
  `busy_elsewhere()` now enforces one connection at a time for every
  radio.
- `src/ui.inc`: **Flood mesh** and **Routed relay** menu entries and
  lists. The connected screen is titled `Flood connected` or
  `Relay connected` and shows the route.
  - The Status page gains a flood/routed line; the LoRa lines are merged
    into one.
  - A **Routes** page shows each board's route over every way.
  - A **Tools** page holds every test setting that was serial-only:
    middleman block, rate, ESP-MESH leave/rejoin/root, mutes.
  - On the Volume page, OK mutes and unmutes.
  - **Nothing needs the serial port.**
- `src/radio.cpp`: the receive queue is 64 deep (flooding brings copies),
  and frames down to 25 bytes are accepted (route messages are short).
- `src/main.cpp`: relay frames are recognised by their first byte, before
  the ESP-NOW handling.
- `src/mesh.inc`: the route wording is shared (`path_text`).

## Screens and buttons

```
Menu ── ESP-NOW ─────▶ (as before)
 ├──── ESP-MESH ────▶ (as before)
 ├──── Flood mesh ──▶ Flood list ─── OK on Board 2 ─▶ Connecting ─accepted─▶ Flood connected
 ├──── Routed relay ▶ Routed list ── OK on Board 2 ─▶ Connecting ─accepted─▶ Relay connected
 ├──── LoRa          (dot: online over that way)        "over flood mesh" /      Route: 1 > 3 > 2, via Board 3
 ├──── Volume        (OK: mute/unmute)                  "over routed relay"      hold OK: talk  RIGHT: hello
 ├──── Status                                                                    CANCEL: disconnect
 ├──── Routes        each board's route over ESP-MESH, flood mesh and routed relay
 └──── Tools         test settings: OK switches, red dot = on
```

The menu has 9 rows, so it scrolls (UP/DOWN). Every page goes back with
CANCEL.

**Status and Routes pages scroll:** with more boards than fit, a scroll
bar shows on the right, and **UP/DOWN** move half a page.

**Routes page:** this board's place in ESP-MESH (root, or layer and
parent; the boards joined through it), then for each other board:

```
Board 2
 Mesh   1 > 2, direct
 Flood  1 > 3 > 2, via Board 3
 Routed 1 > 3 > 2, via Board 3
Blocked: 2  Rate 1 Mbps
```

**Tools page** (OK switches the selected row; a red dot means a test
setting is on; the line above the hints confirms each change):

| Row | Does |
| --- | --- |
| `Block Board N` (one per board) | Ignore flood and routed frames heard directly from board N, as if it were out of range. Set it on both boards to force a middleman |
| `Rate: 1 Mbps` | Switch the ESP-NOW rate between 1 and 6 Mbit/s (set it the same on every board) |
| `Leave mesh` / `Rejoin mesh` | Leave ESP-MESH completely, as if this board were down; then come back |
| `Be mesh root` | Make this board the ESP-MESH root for every board, and keep it root: the others give way and join it (green dot while this board is root) |
| `Mute flood`, `Mute routed`, `Mute mesh`, `Mute LoRa` | Drop everything that way, for link-loss tests |

The connected screen shows the route under the other board's name:

| Route line | Means |
| --- | --- |
| `Route: 1 > 2, direct` (green) | The frames go straight between the two boards |
| `Route: 1 > 3 > 2, via Board 3` (orange) | Board 3 relays. For the flood mesh, this is the path of the first copy of the other board's last frame (every path is used). For the routed relay, it is the route this board sends along |
| `Route: finding...` (grey) | Nothing heard yet (flood), or no route yet (routed) |

## Step-by-step: use and test

Boards 1 and 2 with the handset wiring. Board 3 (no speaker needed) for the
middleman tests. Wiring is unchanged
([handset_lora_integration_test](../handset_lora_integration_test/README.md#wiring)).

### 1. Flash all boards with this build

From this folder. These boards need **BOOT + RESET** before each upload:
hold BOOT, tap RESET, release BOOT, upload, tap RESET. Close any serial
monitors first: an open monitor blocks the port.

| USB serial (`pio device list`, `SER=`) | Board | COM on the PC this was written on |
| --- | --- | --- |
| `5B5F021600` | Board 1 | COM11 |
| `5B5F021597` | Board 2 | COM4 |
| `5B5F021589` | Board 3 (MAC `E0:72:A1:D9:A4:94`) | COM10 |
| `5B42019238` | Board 4 (MAC `E0:72:A1:D8:0E:48`) | COM15 |

```powershell
cd tested_parts\handset_espnow_mesh_integration_test
~\.platformio\penv\Scripts\pio.exe run -e board1 -t upload --upload-port COM11
~\.platformio\penv\Scripts\pio.exe run -e board2 -t upload --upload-port COM4
~\.platformio\penv\Scripts\pio.exe run -e board3 -t upload --upload-port COM10
~\.platformio\penv\Scripts\pio.exe run -e board4 -t upload --upload-port COM15
```

Each board needs its own environment (`boardN`), or it stops with "Wrong
boardN environment".

Steps 2–5 use only the D-pad and the screen. No serial monitor or typed
commands are needed: everything is on the **Routes** and **Tools** pages.

### 2. Check the boards found each other

Reset all boards (RESET button). After about 10 s, on board 1:

1. Menu → **Flood mesh**: `Connect Board 2` has a **green** dot.
2. CANCEL, Menu → **Routed relay**: `Connect Board 2` has a **green** dot.
3. CANCEL, Menu → **Routes**, which shows:

   ```
   Board 2
    Mesh   1 > 2, direct
    Flood  1 > 2, direct
    Routed 1 > 2, direct
   ```

   `finding...` means nothing has been heard yet: wait a few seconds; the
   page updates every second. `offline` means that way doesn't reach the
   board yet.

### 3. Connect over each way

1. Board 1: Menu → **Flood mesh** (or **Routed relay**) →
   `Connect Board 2` → **OK**.
2. Board 2 beeps: "Board 1 wants to connect **over flood mesh**". Press
   **OK** (after half a second).
3. Both show **Flood connected** (or **Relay connected**) with
   `Route: 1 > 2, direct` under the board's name.
4. Hold OK to talk, RIGHT to say hello, CANCEL to disconnect.
5. After the other board talks, the screen's bottom line shows
   `Last heard … s ago` and `N/N frames ok`. `MISSING` there means frames
   were lost.

### 4. Make board 3 the middleman on one desk

Boards in range of each other talk directly. To force the frames through
board 3 without moving anything, make boards 1 and 2 ignore each other's
relayed frames. This affects flood and routed only; ESP-NOW, ESP-MESH and
LoRa are untouched.

1. Board 1: Menu → **Tools** → **Block Board 2** → OK. The dot turns red
   and the line above the hints says `Board 2 blocked`.
2. Board 2: Menu → **Tools** → **Block Board 1** → OK.
3. Board 3 just needs to be on; it relays by itself.
4. Board 1: Menu → **Routes**. Within a few seconds Board 2's
   `Routed` line reads **`1 > 3 > 2, via Board 3`** (orange).
5. Connect over **Routed relay**: the connected screen shows
   **`Route: 1 > 3 > 2, via Board 3`** (orange). Talk: the voice goes
   through board 3.
6. Disconnect, then connect over **Flood mesh**. The route line also
   shows `via Board 3`, because the only copies boards 1 and 2 accept are
   the ones board 3 relayed.
7. Undo: Tools → **Block Board 2** → OK on board 1, and **Block Board 1**
   → OK on board 2. The dots go grey.

With real distance instead (boards 1 and 2 out of each other's range,
board 3 between them) it works the same with no settings: flood copies only
get through via board 3, and route discovery only finds `1 > 3 > 2`.

### 5. Tests on the boards

| Test | Steps | Pass |
| --- | --- | --- |
| T1 Link loss, flood | Connected over the flood mesh. Board 2: Tools → **Mute flood** → OK | Both show **Connection lost** within ~7 s. Mute flood → OK again: board 2's dot turns green again |
| T2 Link loss, routed | The same with **Mute routed** | The same |
| T3 One at a time | Connected over the flood mesh, try connecting over Routed relay, ESP-NOW or ESP-MESH | Refused: `not reachable` notice and a low beep |
| T4 Middleman down, flood | Middleman set up (step 4), connected over the flood mesh, power off board 3 | **Connection lost** within ~7 s (no other path while blocked). Unblock and reconnect: `1 > 2, direct` |
| T5 Middleman down, routed | The same over the routed relay | The call ends; Routes shows Board 2 `Routed offline` (no path while blocked); unblocked, `1 > 2, direct` again |
| T6 Faster radio | If relayed voice shows `MISSING`: on **every** board Tools → **Rate: 1 Mbps** → OK (becomes `Rate: 6 Mbps`) | Fewer or no missing frames. The earlier relay voice test needed 6 Mbit/s |
| T7 ESP-MESH root down | Board 1 (root): Tools → **Leave mesh** → OK | Board 2's Routes page: `Mesh: Looking; root in N s`, then `Mesh: root`. Board 1: **Rejoin mesh** → OK; it joins under board 2 |
| T8 Choose the mesh root | Board 3: Tools → **Be mesh root** → OK | Within about 10 s board 3's Routes page shows `Mesh: root`, the others `parent Board 3` (or through another board). A minute later board 3 is still root |
| T9 Real distance | Boards 1 and 2 out of range of each other, board 3 between, nothing blocked | Both ways connect and show `via Board 3`; on the ESP-NOW list, board 2 is grey |
| T10 Two relays | Four boards in a line (needs board 4's MAC in `network_config.h`) | Route `1 > 3 > 4 > 2, via 2 boards` |

### 6. Optional: scripted test from a PC

You don't need this; it repeats the tests above automatically.
`tools/relay_test.py` drives boards 1 and 2 over USB serial and checks their
logs. Close the serial monitors first; the script reboots both boards.
Port 1 must be board 1.

```powershell
~\.platformio\penv\Scripts\python.exe tools\relay_test.py relay1 COM11 COM4
~\.platformio\penv\Scripts\python.exe tools\relay_test.py relay2 COM11 COM4 sessions middle
```

The second command runs the middleman test; board 3 must be on.

| Test | What it checks |
| --- | --- |
| R1 `sessions` | Flood and routed sessions on both boards, each online over both; board 1 finds a route to board 2 |
| R2 `flood` | Connect/accept over the flood mesh, route `1 > 2` on screen, hello both ways, 5 s voice both ways intact, disconnect |
| R3 `routed` | The same over the routed relay |
| R4 `busy` | During a flood connection, routed, ESP-NOW and ESP-MESH connects are refused |
| R5 `loss` | `flood mute on` / `routed mute on` on board 2: both drop the connection within 15 s; back online after unmuting |
| R6 `middle` (not in the default run) | With `relay block` both ways: route `1 > 3 > 2, via Board 3` found, both ways connect through board 3, screen shows it, 5 s voice both ways intact |

Logs land in `evidence/`. Check the voice streams frame by frame, sender's
log first, both ways:

```powershell
~\.platformio\penv\Scripts\python.exe tools\check_streams.py evidence\relay1_board1.txt evidence\relay1_board2.txt
~\.platformio\penv\Scripts\python.exe tools\check_streams.py evidence\relay1_board2.txt evidence\relay1_board1.txt
```

## Serial commands (optional)

Nothing above needs them; they are the same functions, for scripted tests
and for reading the log. Everything from `handset_espmesh_integration_test`
(`help` lists them), plus:

| Command | Does |
| --- | --- |
| `flood` / `routed` | Status: link, frames sent, relayed, copies dropped, delivered, rejected, blocked, out of hops, send errors (routed: no route, requests), sessions with their routes |
| `flood connect N` / `routed connect N` | Connect to board N that way |
| `routes` | The routed relay's routing table, and the boards blocked (the Routes page shows the same) |
| `relay block N` / `relay unblock N` | Tools → Block Board N |
| `flood mute on\|off` / `routed mute on\|off` | Tools → Mute flood / Mute routed |
| `radio 11 6` / `radio 11 1` | Tools → Rate |
| `mesh off` / `mesh on` / `mesh root` | Tools → Leave mesh / Rejoin mesh / Be mesh root |
| `accept`, `decline`, `disconnect`, `hello`, `talk [S]`, `test [S]`, `stop`, hold OK | Act on the connection in progress, whichever way |

Log lines:
- prefixes: `FLOOD …` and `ROUTED …` (`SESSION`, `PEER`, `LINK`, `TEXT`,
  `ROUTE`, `REJECT`)
- routed relay only: `ROUTED route to N`, `ROUTED looking for a route`,
  `ROUTED route request from N answered`, `ROUTED … route error sent`
- voice: `TX_START` / `RX_START` with `via=flood` or `via=routed`

## Troubleshooting

| You see | Means / try |
| --- | --- |
| Routes page: `Routed offline` for a board that is on | Board 2 isn't on this build, or no path exists. Check the `Blocked:` line at the bottom of the Routes page, and Tools for red dots. It asks again every 5 s |
| `N/N frames MISSING` after relayed voice | At the default 1 Mbit/s, three boards relaying voice is a lot of airtime. Tools → **Rate** to 6 Mbps on every board, as the relay voice test needed (`e2e_relay_voice_test`) |
| A way's dots stay grey | Tools: is that way muted (red dot)? Is another board blocked? Reflash all boards from this folder if a board is on an older build |
| Route line stays `finding...` | Flood: nothing has arrived from the other board yet (wait a ping, 2 s). Routed: no route; see the first row |
| `Stopped` with `Wrong boardN environment` or `This MAC is not in network_config.h` | See [handset_espmesh_integration_test](../handset_espmesh_integration_test/README.md#troubleshooting) |

ESP-MESH (root failover) works exactly as in
[handset_espmesh_integration_test](../handset_espmesh_integration_test/README.md).
Its serial-only test commands are on the Tools page here (Leave mesh,
Rejoin mesh, Be mesh root, Mute mesh), and its topology is on the Routes
page. `tools/mesh_test.py` runs on this build too.
