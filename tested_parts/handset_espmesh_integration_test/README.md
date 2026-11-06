# Handset ESP-MESH integration: ESP-NOW and ESP-MESH voice plus LoRa hello/bye

A copy of [handset_lora_integration_test](../handset_lora_integration_test/)
with the ESP-NOW functions duplicated over **ESP-MESH** (ESP-WIFI-MESH,
`esp_mesh.h`). **No Wi-Fi router is used or needed.**

Everything from the LoRa build is still here and unchanged in use: ESP-NOW
connections with push-to-talk voice, LoRa hello/bye texts. ESP-MESH adds a
third connection type with **the same functions as ESP-NOW**:

| Function | ESP-NOW | ESP-MESH (new) |
| --- | --- | --- |
| Pick a board and ask to connect | Menu → ESP-NOW → Connect Board N | Menu → **ESP-MESH** → Connect Board N |
| Accept / decline | OK / CANCEL on the request screen | same ("wants to connect **over ESP-MESH**") |
| Push-to-talk Opus voice | hold OK | hold OK |
| Hello text | RIGHT | RIGHT |
| Volume | UP / DOWN | UP / DOWN |
| Disconnect | CANCEL (both sides drop) | same |
| Link loss / reboot ends the connection | yes | yes |
| Green "online" dot | presence pings | presence pings over the mesh |
| End-to-end encryption | session key per peer | **its own** mesh session key per peer |

**One connection at a time, over any radio.** While an ESP-NOW, ESP-MESH or
LoRa connection is asking, asked or connected, a request over another radio
gets "Busy", and connecting over another is refused.

**Status: all five environments build. Not yet tested on hardware.** The
host-tested modules (`call`, `voice_link`, `lora_text`, `audio_dsp`,
`command_parser`, crypto) are unchanged from the LoRa build. Run the
[step-by-step guide](#step-by-step-use-and-test) on boards 1 and 2 first.
**Any board can go down**, the root included: see
[When a board goes down](#when-a-board-goes-down).

## How ESP-MESH works here without a router

ESP-WIFI-MESH normally elects as root the board with the best signal to a
Wi-Fi router, and the root links the mesh to that router. This project has
no router, so:

- **Nobody votes; the boards take turns.** Every board calls
  `esp_mesh_fix_root(true)`, so the mesh never runs its router-based vote.
  The firmware picks the root instead: board 1 (`network_config::mesh_root_id`,
  the *preferred* root) when it is up, else the next board in ID order that
  is. The root calls `esp_mesh_set_type(MESH_ROOT)`, as in ESP-MDF's
  router-less setup.
- **The router SSID is empty.** The root never connects anywhere. It
  reports each look for the absent router as a disconnect; that is counted
  (`router misses` in `mesh`) and logged once:
  `MESH root: no router to join ..., as intended`. If this IDF ever refuses
  an empty SSID, the firmware sets `SE19-NO-ROUTER`, a name no router has,
  and says so at boot. Either way no router is joined.
- **The other boards join the root**, or a board already joined to it, up to
  6 hops (`mesh_max_layer`). Two boards out of each other's range can talk
  through a third: the mesh forwards the frames.
- **Channel 11 for both.** ESP-NOW and the mesh share the one Wi-Fi radio,
  so the mesh is fixed to ESP-NOW's channel (`allow_channel_switch = false`).
  `radio CH MBPS` can still change the rate, but not the channel.
- **Security.** The mesh soft-AP is WPA2. Its password is derived from the
  network key in `../shared_keys/e2e_keys.h`, so every board has it and
  nothing else does. On top of that every frame is sealed end to end exactly
  as over ESP-NOW (AES-GCM, network-key hop tag). Each peer gets a **mesh
  session key of its own**, made by the same handshake over the mesh, so a
  frame recorded on one radio doesn't open on another.

### When a board goes down

Nothing waits forever and nothing stops the board. ESP-NOW and LoRa don't
depend on the mesh, so they keep working throughout.

| Board that goes down | What the others do |
| --- | --- |
| A node (not the root, no boards behind it) | Its dot turns grey within ~6.5 s (presence pings stop, or sends to it keep failing). A connection with it ends: **Connection lost**. Asking it ends with **Not reachable** |
| A middleman (boards joined through it) | As above, and the boards behind it lose their parent and rejoin through any other board in range, by themselves |
| **The root** | Its connections end as above. Every other board starts looking for a mesh; the **first board in the order still up takes over as root** after its turn (board 2 after 14 s, board 3 after 22 s), and the rest join it. Connect again once the dots are green |
| Any board coming back | It looks for a mesh first and **joins the one that's there**, so there is never a second root. It doesn't take the root back, except the board **chosen** as root (below), which joins and then takes the root back |

**How the takeover works.** Every board starts as a node and looks for a
mesh to join. It becomes the root only if it finds none, or its mesh has
lost its root, for its turn's wait:

| Board | Turn | Becomes root after looking for |
| --- | --- | --- |
| Board 1 (preferred) | 1st | 6 s (`mesh_root_wait_ms`) |
| Board 2 | 2nd | 14 s (+8 s per place, `mesh_root_wait_step_ms`) |
| Board 3 | 3rd | 22 s |

So at power-on board 1 forms the mesh 6 s in and the others join it. With
board 1 down, board 2 takes over and board 3 joins it, before its own
turn comes. Changing `mesh_root_id` changes who is first; every board must
agree.

**Choosing the root.** On any board, Tools → **Be mesh root** makes that
board the root for every board, and it stays root:

1. It becomes root at once.
2. It tells every board over ESP-NOW, so even boards in another mesh hear
   of it.
3. The root in charge gives way and joins it. The others follow, since
   their root moved.

The chosen board is first in the takeover order from then on. If it goes
down, the next board takes over as usual. When it comes back, it joins and
then takes the root back. The choice lasts until another board is chosen,
or until every board has been reset.

While the chosen board keeps saying over ESP-NOW that it is root, every
board **regroups under it**:

- A board looking for a mesh looks again rather than become a second root.
- A root without the chosen board in its mesh gives way and joins it, and
  its boards follow.

So moving the root never leaves the boards split into two meshes. If the
chosen root's mesh can't be reached after 3 tries, 20 s apart, the usual
takeover applies, and they try again every 2 minutes.

**Two roots (a split).** If a board became root while out of range of the
others, then comes back, there are two meshes. A root that nobody has
joined for 30–45 s (`mesh_lonely_root_ms`, plus a random part so two lonely
roots don't step down together) steps down and looks for another mesh to
join, so the boards end up in one mesh again. It does this only if another
board is in range (online over ESP-NOW). With none, it would find no mesh
and take the root back, over and over. The chosen root never steps down. This covers any split with up
to 3 boards. With 4 or more, two roots that both have boards joined would
stay split until one of them goes down.

**Restarts never cut voice.** Becoming root, stepping down and retrying a
mesh that failed all restart the mesh for a moment, so none of them happens
while voice is being sent or played; stepping down also waits until no
connection is in progress. A restart takes the receive task out of
`esp_mesh_recv()` first, because the mesh's stop deletes the queue it
waits on, and the send task out of `esp_mesh_send()`.

**Voice through the root.** Between two boards under the root (route
`1 > 3 > 2`), ESP-MESH carries about 30 packets a second, and one send can
take a few hundred ms. Voice is 50 frames a second, so:

- **A send task:** frames go to the mesh from their own task, never from
  the loop. Measured 2026-11-06: before this, the loop stalled, frames
  were dropped, the screen hung on TALKING, and connections dropped.
- **One packet per two frames:** each voice packet already carries the
  frame before it, so over the mesh only every second frame is sent.
  Every frame still arrives, at 25 packets a second, 20 ms later.
  Measured: 500/500 frames intact both ways.
- **Per-hop retries stay on:** voice keeps the mesh's own per-hop
  retransmission (`MESH_TOS_P2P`). Without it (`MESH_TOS_DEF`), no voice
  frame arrived at all between the two nodes.

The `mesh` line shows `send queue full N, slowest send N ms`. A slow board
(hundreds of ms) has a weak link to its parent: move it closer.

### What changed from handset_lora_integration_test

- `include/mesh_radio.h`, `src/mesh_radio.cpp`: the mesh. It starts, restarts
  as root or node, sends without blocking, and runs a receive task feeding a
  queue. It turns mesh events into notes for the loop to print, and tracks
  which board is root now.
- `src/mesh.inc`: the ESP-NOW connection layer duplicated for the mesh:
  sessions, presence, connect/accept/decline, receive dispatch, and the
  `mesh` serial commands. It also holds the root failover policy.
- `src/handset.inc`: the voice and text code is shared and now carries
  which link a stream uses (`via::espnow` / `via::mesh`). `TX_START` and
  `RX_START` lines gain a `via=` field. Every other ESP-NOW log line is
  unchanged.
- `src/radio.cpp`: Wi-Fi now starts through ESP-IDF, not Arduino's `WiFi`
  class. Arduino's event handler answers the first station disconnect with
  `WiFi.disconnect(); WiFi.begin();`, which would knock a mesh node off its
  parent.
- `src/ui.inc`: an ESP-MESH menu entry and list. The connected screen is
  shared (titled `Mesh connected`). The Status page gains two mesh lines.
  - A **Routes** page shows where this board is in the mesh and the route
    to each board.
  - A **Tools** page holds every test setting that was serial-only: rate,
    leave/rejoin mesh, become root, mute mesh and LoRa.
  - On the Volume page, OK mutes and unmutes.
  - **Nothing needs the serial port.**
- ESP-NOW's "busy" flag is now refreshed every loop pass, as LoRa's is. In
  the LoRa build it was set only when a request arrived, so after answering
  one "busy" a later ESP-NOW `connect` could be refused until another
  request came in.

## Screens and buttons

```
Menu ── ESP-NOW ──▶ ESP-NOW list ── OK on Board 2 ──▶ (ESP-NOW, as before)
 │
 ├──── ESP-MESH ──▶ ESP-MESH list ─ OK on Board 2 ──▶ Connecting ─accepted─▶ Mesh connected
 │   (dot: green     (dot: online                      "over ESP-MESH"        hold OK: talk
 │    in the mesh,    over the mesh)                                          RIGHT: hello
 │    red failed,                                                             UP/DOWN: volume
 │    grey joining)                                                           CANCEL: disconnect
 ├──── LoRa ──────▶ LoRa list ────── (LoRa, as before)
 ├──── Volume        (OK: mute/unmute)
 ├──── Status
 ├──── Routes        this board's place in the mesh, the mesh route to each board
 └──── Tools         test settings: OK switches, red dot = on
```

The menu has 7 rows, so it scrolls (bar on the right). Every page goes
back with CANCEL. The ESP-MESH list's bottom line says where this board
is:
- `Root, 2 boards`
- `Layer 2, root Board 1`
- `Looking; root in 9 s` (counting down to this board's takeover)
- `Mesh off (Tools: Rejoin mesh)`

**Status page scrolls:** with more boards than fit, a scroll bar shows on
the right, and **UP/DOWN** move half a page.

**Routes page:**
- **This board:** root, or its layer and parent
- **Root:** which board is root
- **Children:** the boards joined through this one
- **Each other board:** its mesh route, for example
  `Board 2: 1 > 3 > 2, via Board 3` in orange, or `offline`
- **Rate:** the ESP-NOW rate

It updates every second.

**Tools page:** OK switches the selected row. A red dot means a test
setting is on, and the line above the hints confirms each change.

| Row | Does |
| --- | --- |
| `Rate: 1 Mbps` | Switch the ESP-NOW rate between 1 and 6 Mbit/s (set it the same on every board) |
| `Leave mesh` / `Rejoin mesh` | Leave the mesh completely, as if this board were down (another takes over if it was root); then come back |
| `Be mesh root` | Make this board the root for every board, and keep it root: the others give way and join it (green dot while this board is root) |
| `Mute mesh`, `Mute LoRa` | Drop everything that way, as out of range would, for link-loss tests |

The connected (hold OK to talk) screen shows **the route** under the
other board's name, kept up to date during the call:

| Route line | Means |
| --- | --- |
| `Route: 1 > 3 > 2, via Board 3` (orange) | Over the mesh, through board 3 as middleman |
| `Route: 1 > 2, direct` (green) | Over the mesh, one hop (one of the two is the other's parent) |
| `Route: 1 > 2, direct (ESP-NOW)` (green) | An ESP-NOW connection: always direct |
| `Route: finding...` (grey) | Not known yet: each board says where it hangs in its presence pings, every 2 s |

The mesh is a tree, so frames go up from one board to the nearest board
both hang from, then down to the other. Each board knows only its own
parent; the others' come in their mesh presence pings (parent and layer,
sealed like everything else), and each board works out the path from
those. A move (a middleman coming or going) pings at once.

## Wiring

Unchanged from [handset_lora_integration_test](../handset_lora_integration_test/README.md#wiring):
mic GPIO1, speaker GPIO8, LCD, D-pad, Core1121 on GPIO5/6/7/15/16/17/18.
ESP-MESH uses the board's own Wi-Fi, so it needs no wiring.

## Step-by-step: use and test

You need boards 1 and 2, each with the handset wiring. A third board (no
speaker needed) is only needed for middleman, multi-hop and "Busy" tests.

### 1. Build and flash

From this folder. These boards need **BOOT + RESET** before each upload:
hold BOOT, tap RESET, release BOOT, then upload, then tap RESET.

**Find which port is which board first.** Windows COM numbers differ per
PC and can change when you replug, but each board's USB serial number
doesn't. Run `pio device list`:

| USB serial (`SER=`) | Board | macOS port |
| --- | --- | --- |
| `5B5F021600` | Board 1 (preferred mesh root) | `/dev/cu.usbmodem5B5F0216001` |
| `5B5F021597` | Board 2 | `/dev/cu.usbmodem5B5F0215971` |
| `5B5F021589` | Board 3 (MAC `E0:72:A1:D9:A4:94`) | |
| `5B42019238` | Board 4 (MAC `E0:72:A1:D8:0E:48`) | COM15 |

On the PC this was first run on, that was COM11 = board 1, COM4 =
board 2 and COM10 = board 3. The commands below use those; use your own.

Windows (PlatformIO's own `pio`; use your COM ports from Device Manager):

```powershell
~\.platformio\penv\Scripts\pio.exe run -e board1 -t upload --upload-port COM11
~\.platformio\penv\Scripts\pio.exe run -e board2 -t upload --upload-port COM4
```

macOS (the ports the other projects use):

```sh
~/.platformio/penv/bin/pio run -e board1 -t upload --upload-port /dev/cu.usbmodem5B5F0216001
~/.platformio/penv/bin/pio run -e board2 -t upload --upload-port /dev/cu.usbmodem5B5F0215971
```

In VS Code you can instead pick the `board1` / `board2` environment in the
PlatformIO toolbar and press Upload. **Each board must get its own
environment**: the firmware checks its MAC against `network_config.h` and
stops with "Wrong boardN environment" otherwise.

Steps 2–4 use only the D-pad and the screen. No serial monitor or typed
commands are needed.

### 2. Check the mesh formed

Press RESET on all boards. Within about 30 s:

1. **Board 1:** Menu → **Routes** shows `Mesh: root, 2 boards`,
   `Root: Board 1`, `Children: Board 2`, `Board 2: 1 > 2, direct` (green).
2. **Board 2:** Menu → **Routes** shows `Mesh: layer 2, parent Board 1`,
   `Root: Board 1`, `Board 1: 2 > 1, direct`.
3. **Both:** the menu's ESP-MESH dot is **green**. On the ESP-MESH list,
   `Connect Board N` has a green dot.

Board 2 must **not** show `Mesh: root`. That would mean it didn't find
board 1's mesh in time and became a second root. It sorts itself out
within about a minute; see Troubleshooting if it doesn't.

Optional, with a serial monitor (921600 baud) on the PC, the boot log shows
the same:

| Board 1 (becomes root) | Board 2 (node) |
| --- | --- |
| `MESH starting \| channel 11 \| ... this board's turn after 6 s` | `MESH starting \| ... this board's turn after 14 s` |
| `MESH started \| looking for a mesh to join; ... root after 6 s if none` | `MESH started \| looking for a mesh to join; ... after 14 s if none` |
| `MESH no mesh with a root for 6 s ...: becoming the root` | |
| `MESH started as root (no router)` | `MESH joined \| parent 1 (Board 1) \| layer 2` |
| `MESH root: no router to join ..., as intended` | `MESH root is Board 1` |
| `MESH child joined: 2 (Board 2)` | |
| `MESH SESSION established with 2` | `MESH SESSION established with 1` |
| `MESH PEER 2 (Board 2) online` | `MESH PEER 1 (Board 1) online` |

### 3. Connect and talk over the mesh

1. Board 1: Menu → **ESP-MESH** → `Connect Board 2` → **OK**.
2. Board 2 beeps and shows **"Board 1 wants to connect over ESP-MESH"**.
   Press **OK** (after half a second; earlier presses are ignored on purpose).
3. Both show **Mesh connected**, with the route under the other board's
   name (`Route: 1 > 2, direct`, or `… via Board 3` through a middleman).
4. **Hold OK** on board 1 and speak; board 2 plays it (box: "Board 1 is
   talking"), then a roger beep. Then the other way round.
5. **RIGHT** on board 1: board 2 shows "Hello from Board 1" in the banner.
6. **CANCEL** on either board: both show **Disconnected**.
7. After the other board talks, the screen's bottom line shows
   `Last heard … s ago` and `N/N frames ok`. `MISSING` there means frames
   were lost.

### 4. Tests on the boards

| Test | Steps | Pass |
| --- | --- | --- |
| T1 Link loss | Connected over the mesh. Board 2: Tools → **Mute mesh** → OK | Both show **Connection lost** within ~10 s. **Mute mesh** → OK again: board 2's dot turns green again on board 1's ESP-MESH list |
| T2 Root down | Board 1 (root): Tools → **Leave mesh** → OK | Board 2's Routes page: `Mesh: Looking; root in N s`, then about 14 s later `Mesh: root, 1 board` |
| T3 Root back | After T2, board 1: Tools → **Rejoin mesh** → OK | Board 1's Routes page: `Mesh: layer 2, parent Board 2`, never `Mesh: root` (it joins, there is never a second root). Connect 1 → 2 over ESP-MESH works |
| T4 Choose the root | Board 2 (a node): Tools → **Be mesh root** → OK | Within about 10 s board 2's Routes page shows `Mesh: root`, board 1's `parent Board 2`. A minute later board 2 is still root |
| T5 One at a time | Connected over the mesh, try connecting over ESP-NOW or LoRa | Refused: `not reachable` notice and a low beep |
| T6 Middleman | Board 3 on, placed so board 2 can't reach board 1 directly | Board 2's Routes page: `parent Board 3`. Board 3: `Children: Board 2`. Connected screen: `Route: 1 > 3 > 2, via Board 3` |
| T7 Faster radio | If ESP-NOW voice shows `MISSING`: on **every** board Tools → **Rate: 1 Mbps** → OK (becomes `Rate: 6 Mbps`) | Fewer or no missing frames |

### 5. Optional: scripted test from a PC

You don't need this; it repeats the tests above automatically, with the
serial log as evidence. `tools/mesh_test.py` drives both boards over USB
serial and checks their logs. Close any serial monitors first. The script
reboots both boards (`reboot`) so each log starts at boot. Port 1 must be
board 1. A full run takes about 10 minutes.

```powershell
~\.platformio\penv\Scripts\python.exe tools\mesh_test.py mesh1 COM11 COM4
```

It prints PASS/FAIL per check and `N/M checks passed`, and saves both logs to
`evidence/mesh1_board1.txt` and `_board2.txt`. Run single tests by name:
`... mesh1 COM11 COM4 voice loss`. Afterwards, check every voice stream
frame by frame (sender's log first, so run it both ways):

```powershell
~\.platformio\penv\Scripts\python.exe tools\check_streams.py evidence\mesh1_board1.txt evidence\mesh1_board2.txt
~\.platformio\penv\Scripts\python.exe tools\check_streams.py evidence\mesh1_board2.txt evidence\mesh1_board1.txt
```

| Test | What it checks |
| --- | --- |
| M1 `join` | Board 1 looks first, then becomes root; board 2 joins it and never becomes a second root; mesh sessions on both, each sees the other online; ESP-NOW session alongside |
| M2 `connect` | Mesh connect/accept; both on the connected screen for ESP-MESH; ESP-NOW and LoRa connect refused meanwhile; disconnect drops both |
| M3 `hello` | Hello both ways over the mesh |
| M4 `voice` | 5 s, 5 s and 10 s test-signal streams both ways, every frame played, CRC match |
| M5 `dpad` | The same with serial button presses: list, OK, accept, RIGHT hello, hold OK 3 s to talk, CANCEL |
| M6 `loss` | `mesh mute on` on board 2: both drop the connection within 15 s; back online after `mesh mute off`; reconnects |
| M7 `espnow` | ESP-NOW connect + 5 s voice still works in this build; mesh connect refused meanwhile |
| M8 `reboot` | Board 2 reboots during a mesh connection: board 1 ends it; board 2 rejoins, new session, connects again |
| M9 `rootdown` | The root goes down (`mesh off`): the other board logs `becoming the root` within 45 s; the old root comes back (`mesh on`) as a node under it, not a second root; both online; connect and a 5 s voice stream work |
| M10 `choose` | The node is chosen as root (`mesh root`): the old root hears of it over ESP-NOW, gives way and joins it; 50 s later it is still root; both online; connect works |

### 6. More tests by hand (power and distance)

| Test | Steps | Pass |
| --- | --- | --- |
| H1 Root off | Power off board 1 while boards 2 and 3 are idle | Boards 2 and 3: their ESP-MESH lists and Routes pages count down `Looking; root in N s`. About 14 s later board 2's Routes page shows `Mesh: root`; board 3's shows `parent Board 2`. Board 2 → ESP-MESH → Board 3 connects. ESP-NOW and LoRa work throughout |
| H1b Root back | After H1, power board 1 on | Board 1's Routes page: `Mesh: layer 2`, never `Mesh: root`; all three dots green |
| H2 Root off mid-call | Boards 1 and 2 connected over mesh, power off board 1 | Board 2 shows **Connection lost** within ~10 s, takes over as root ~14 s later, and connects to board 3 if it is up |
| H2b Middleman off | Board 3 as the middleman (board 2's Routes: `parent Board 3`; board 3's: `Children: Board 2`), power board 3 off | Board 2 rejoins through board 1 if in range (Routes: `parent Board 1`); a call through board 3 ends with **Connection lost** |
| H3 Node out of range | Connected over mesh, walk board 2 away | Voice degrades, then **Connection lost**; back in range, board 2 rejoins (Routes: `Mesh: layer 2`) |
| H4 Busy (needs board 3 with a screen) | Boards 1 and 2 connected over mesh; board 3 asks board 1 over ESP-NOW, ESP-MESH or LoRa | Board 3 shows **Busy** |
| H5 Multi-hop (needs board 3) | Board 1 (root) in the middle, boards 2 and 3 at either end, out of each other's ESP-NOW range | Board 2 → ESP-MESH → Board 3 connects and voice plays; board 1's Routes page shows `Mesh: root, 3 boards` |
| H6 LoRa unchanged | LoRa tests L1–L11 from [handset_lora_integration_test](../handset_lora_integration_test/README.md#hardware-tests-boards-1-and-2) (Menu → LoRa; for its mute steps use Tools → **Mute LoRa**) | Same results as there |

## Serial commands (optional)

Nothing above needs them; they are the same functions, for scripted tests
and for reading the log.

| Command | Same as on screen |
| --- | --- |
| `mesh off` / `mesh on` / `mesh root` | Tools → Leave mesh / Rejoin mesh / Be mesh root |
| `mesh mute on\|off`, `lora mute on\|off` | Tools → Mute mesh / Mute LoRa |
| `radio 11 6` / `radio 11 1` | Tools → Rate |
| `mesh` | Routes page (plus frame counters) |

Everything from `handset_lora_integration_test` (`help` lists them), plus:

| Command | Does |
| --- | --- |
| `mesh` | Mesh state: root/node, layer, which board is root, this board's parent and the boards joined to it (`parent Board 3 \| children 1 (Board 2)`: a middleman has both), boards in the mesh, link, frames sent/failed/received/dropped, accepted/rejected, parent joins/losses, router misses, restarts, sessions |
| `mesh connect N` | Connect to board N over ESP-MESH |
| `mesh mute on\|off` | Drop every mesh frame sent and received, as out of range would (the mesh itself stays up), for link-loss tests |
| `mesh off` | Leave the mesh completely, as if this board were down: no soft-AP, no forwarding. If it was the root, another takes over |
| `mesh on` | Come back: look for the mesh and join it, as at boot (root only if none is found in this board's turn) |
| `mesh root` | Tools → Be mesh root: make this board the root for every board; the others give way and join it, and it stays root |
| `accept`, `decline`, `disconnect` | Act on the connection in progress, over whichever radio |
| `hello`, `talk [S]`, `test [S]`, `stop`, hold OK | Use the connection in progress, ESP-NOW or ESP-MESH |

Mesh log lines start with `MESH`: `MESH joined`, `MESH SESSION`,
`MESH PEER`, `MESH LINK`, `MESH ROUTE` (the connection's route when first
known and whenever it changes, e.g. `MESH ROUTE to 2 (Board 2): 1 > 3 > 2,
via Board 3`), `MESH TEXT`, `MESH REJECT`. `mesh` also shows the route to
each online board (`2=38CA40BF (route 1 > 3 > 2, via Board 3)`), and `ui` on
the connected screen ends with the route line. Voice lines are the
shared `TX_START` / `RX_START` (with `via=mesh` or `via=espnow`),
`TX_END` / `RX_END`.

## Troubleshooting

| You see | Means / try |
| --- | --- |
| `Stopped` with `Wrong boardN environment for this board` (the log line before it says `Built for device X, but this board is device Y`) | The firmware for board X went onto board Y: the ports were swapped. Identify the ports by USB serial ([step 1](#1-build-and-flash)) and upload `boardY` to that board |
| Boot log starts with `Handset LoRa integration` and has no `MESH` lines | That board still runs the LoRa build: its upload didn't happen (missed BOOT + RESET, or the wrong port) |
| Board 2 logs `becoming the root` although board 1 is on | It didn't find board 1's mesh within its 14 s: board 1 not flashed with this build, out of range, or the mesh slow to form. Every board must use the same `mesh_id`, `mesh_root_id` and channel: reflash all from this folder. Two roots in range merge by themselves within about a minute (the lonely one steps down) |
| `MESH no mesh found yet` repeating, nobody becomes root | Look at the countdown on the Routes page or the ESP-MESH list: a takeover waits while voice plays. If it never comes, send the log |
| `MESH <step> failed: ...`, red ESP-MESH dot | The mesh didn't (re)start. It is tried again every 10 s (`MESH trying again`); the step and error are in the log and on the Status page; ESP-NOW and LoRa still work. If it keeps failing, send the boot log |
| Serial `mesh` (optional) shows `unknown` frames > 0 | Mesh frames from a MAC not in `network_config.h`: a board not provisioned there |
| `MESH warning: Wi-Fi power save is ...` | The mesh turned modem sleep back on; ESP-NOW voice may stutter. Report it with the log |
| `RADIO refused: ESP-MESH holds channel 11` | `radio CH MBPS` with another channel; only the rate can change here |
| Mesh voice drops frames but ESP-NOW doesn't | Between two non-root boards, mesh frames pass through the root (one more hop); board 2 to the root is direct. Compare `RX_END` counters with `check_streams.py`; try `buffer 500` |
