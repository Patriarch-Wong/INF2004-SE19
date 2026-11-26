# Handset peer mesh integration: every board a node, routes both ways

A copy of [handset_espmesh_autoroot_integration_test](../handset_espmesh_autoroot_integration_test/)
(the newest build) with **one addition: the peer mesh**, a fourth way to
connect, next to ESP-NOW, ESP-MESH and LoRa.

In the peer mesh **every board is a node and a router**. Any board can send
to any other, and any board passes traffic on for the others. There is no
root, no router and no board in charge. **Every route works both ways over
the same boards**: if board 1 reaches board 5 as `1 > 4 > 5`, board 5
reaches board 1 as `5 > 4 > 1`.

Everything else is unchanged and works as before: ESP-NOW, ESP-MESH (with
its elected root), LoRa, the broadcasts, the D-pad and screen, encryption.

| Way | Topology | Reaches a board out of range | Who relays |
| --- | --- | --- | --- |
| ESP-NOW | direct links | no | nobody |
| ESP-MESH | tree, elected root | yes, up and down the tree | parents and the root |
| **Peer mesh** | **mesh, no root** | **yes, along the best route** | **any board on the route** |
| LoRa | direct links | no | nobody |

**Status: host tests pass, including 300 random five-board topologies.
All five environments build with PlatformIO (2026-11-10, no warnings from
the project's sources). Not yet tested on hardware.**

## Topology

```text
          B3 --------- B2
         /  \         / |      Every line is an ESP-NOW link both boards hear.
       B1    \       /  |      B1 to B5: 1 > 4 > 5
         \    \     /   |      B5 to B1: 5 > 4 > 1 (the same boards)
          `--- B4 ------ B5    B1 to B2: 1 > 3 > 2, and back 2 > 3 > 1
```

Every board hears some of the others directly. A frame for a board out of
range is handed from board to board along the route.

## How it works

### 1. Neighbours

Every board broadcasts an **advert** every 2 s (`peer_advert_ms`, ±200 ms
jitter). A board that hears another's advert (or any peer mesh frame from
it) directly counts it as a **neighbour**. It keeps a **quality** per
neighbour, 0–100: how much of that neighbour it heard lately. Each period
the quality moves a quarter of the way to 100 if heard, or to 0 if not. A
new neighbour starts at 50, so it earns trust over about 10 s. A neighbour
below 25, or not heard for 7 s, is gone.

### 2. Adverts: every board learns every link

An advert lists the board's neighbours and their qualities. It is
**flooded**: each board passes each advert on once, with one hop fewer.
So every board holds the same map of every link in the network.

### 3. Links work both ways or not at all

A link is used only if **both** boards list each other. A one-way link
(A hears B, B doesn't hear A) would swallow frames, so it is ignored.

Each link costs **10**, plus half the quality missing from its weaker
direction (10 to 47). The cost is the same both ways.

| Link | Cost |
| --- | --- |
| Heard 100% | 10 |
| Heard 80% | 20 (ties with two perfect hops; fewer hops wins) |
| Heard 50% | 35 |

So two strong hops beat one weak direct link, but a solid direct link
always wins.

### 4. Routes are the same both ways

Each board works out the cheapest route to every board (Dijkstra). The
route between boards A and B is **always worked out from the lower ID's
side**. Ties go to fewer hops, then to the lower board. Every board holds
the same map, so A's route to B is exactly B's route to A turned round.
Replies, acknowledgements and the other person's voice retrace the same
boards.

### 5. Frames carry their route

The sender puts the **whole route** in the frame. Each board on it hands
the frame to the next as **acknowledged ESP-NOW unicast**: the radio
retries each hop. Boards off the route stay quiet.

### 6. Healing

| What happens | What the mesh does | How fast |
| --- | --- | --- |
| A relay stops acknowledging | The board before it drops it as a neighbour, sends a fresh advert at once, and splices its own route into frames already on their way | ~2 s (ESP-NOW ack loss) |
| A board switches off | Its adverts stop; everyone routes around it | ≤ 7–9 s |
| A board comes back | It is trusted gradually; the shorter route returns | ~10–15 s |
| A better path appears | Taken at the next route update | ≤ 2 s after the adverts show it |

A frame can take at most 5 hops (a route of 6 boards, `peer_max_hops`).

### Voice over the peer mesh

Voice and hello are the same as over ESP-NOW (hold OK to talk, RIGHT
hello, UP/DOWN volume). As over ESP-MESH, every other Opus frame is sent
carrying the one before it, so each relay sends half the packets. The
largest sealed voice frame (212 bytes) fits with a full 6-board route (a
compile-time check).

Rough airtime at 1 Mbps: 25 packets/s × ~2.7 ms per hop, so a 3-hop route
uses about 20% of the channel. That's comfortable. Tools → Rate 6 Mbps
cuts it to a few percent.

Talk is push-to-talk, one person at a time, as on every other way. Both
sides can talk in turn; the route is the same in each direction.

### Security

- Every frame is sealed end to end exactly as over ESP-NOW (AES-GCM),
  under a **peer mesh session key of its own per peer**, made by the same
  handshake over the peer mesh.
- A relay checks the network-key hop tag before passing a frame on, and
  can't open it.
- Adverts carry an HMAC under the network key, so a board without the key
  can't forge links or routes.
- Frames from MACs not in `network_config.h` are dropped by the radio
  before any of this.
- Known limit: adverts carry a sequence number but no timestamp, so a
  recorded advert replayed after the original expired (9 s) could show a
  stale link for one advert lifetime. The end-to-end seal is unaffected.

## Screens and buttons

Menu: ESP-NOW, ESP-MESH, **Peer mesh**, LoRa, Broadcast, LoRa broadcast,
Volume, Status, Routes, Tools (10 rows; it scrolls).

| Screen | What's new |
| --- | --- |
| Menu | **Peer mesh** row |
| Peer mesh | `Connect Board N` per board. Green dot: online over the peer mesh (a session, presence pings and a route) |
| Connected | Titled `Peer connected`; the route under the name, e.g. `Route: 1 > 4 > 5, via Board 4` (orange) or `1 > 3, direct` (green) |
| Status | `Peer online: 2 ok 3 - …` and `Peer heard: …` (direct neighbours) |
| Routes | Now scrolls (**UP/DOWN**). `Peer heard: 3:91% 4:88%` (neighbours and quality), then for each board its `Mesh …` and `Peer …` routes, then `Peer blocked: …` |
| Tools | New rows: `Block Board N` (one per board) and `Mute peer mesh` |

`Block Board N` makes this board ignore peer mesh frames heard directly
from board N, as if it were out of range. Set it on **both** boards to
force traffic through a third board on one desk.

## Step-by-step: use and test

Boards 1 and 2 with the handset wiring. Board 3 (no speaker needed) for
the middleman tests. Boards 4 and 5 are optional; every list includes them.
Wiring is unchanged
([handset_lora_integration_test](../handset_lora_integration_test/README.md#wiring)).

### 1. Build, then flash all boards

Build first, from this folder:

```powershell
cd tested_parts\handset_peer_mesh_integration_test
~\.platformio\penv\Scripts\pio.exe run
```

On macOS or Linux: `cd tested_parts/handset_peer_mesh_integration_test`,
then `pio run`, and `pio run -e boardN -t upload --upload-port /dev/cu.usbmodemXXXX`
to flash.

Then flash. These boards need **BOOT + RESET** before each upload: hold
BOOT, tap RESET, release BOOT, upload, tap RESET. Close serial monitors
first.

| USB serial (`pio device list`, `SER=`) | Board | COM on the PC this was written on |
| --- | --- | --- |
| `5B5F021600` | Board 1 | COM11 |
| `5B5F021597` | Board 2 | COM4 |
| `5B42019291` | Board 3 (MAC `E0:72:A1:D8:F6:20`; differs from the other projects' board 3, `5B5F021589`) | check `pio device list` |
| `5B42019238` | Board 4 (MAC `E0:72:A1:D8:0E:48`) | COM15 |
| not seen yet | Board 5 (MAC `E0:72:A1:D5:64:0C`) | check `pio device list` |

```powershell
~\.platformio\penv\Scripts\pio.exe run -e board1 -t upload --upload-port COM11
~\.platformio\penv\Scripts\pio.exe run -e board2 -t upload --upload-port COM4
~\.platformio\penv\Scripts\pio.exe run -e board3 -t upload --upload-port COM10
~\.platformio\penv\Scripts\pio.exe run -e board4 -t upload --upload-port COM15
~\.platformio\penv\Scripts\pio.exe run -e board5 -t upload --upload-port COMx
```

**Flash every board from this folder.** A board on an older build doesn't
send adverts, so the others can't route through it.

### 2. Check the mesh forms

1. Reset all boards at about the same time.
2. Within ~5 s each board's serial log prints `PEER routes | 2: 1 > 2,
   direct | 3: … | neighbours: 2 (63%) 3 (63%)`.
3. Menu → **Routes** on each board: every other board has a `Peer` line.
4. Menu → **Peer mesh**: the other boards' dots turn green (sessions take
   a second or two after the routes).

### 3. Tests on the boards

| Test | Steps | Pass |
| --- | --- | --- |
| P1 Direct | Boards 1 and 2 close together. Menu → Peer mesh → Connect Board 2, accept on 2. Hold OK on 1, then on 2 | Both hear each other. Route `1 > 2, direct` on both |
| P2 Middleman | Tools → **Block Board 2** on board 1, **Block Board 1** on board 2 (board 3 on). Connect 1 to 2 over the peer mesh | Route `1 > 3 > 2, via Board 3` on board 1 **and** `2 > 3 > 1, via Board 3` on board 2. Voice works both ways |
| P3 Same both ways | Any layout with boards 4 and 5 too. Menu → Routes on each board | For every pair, A's `Peer` route is B's turned round |
| P4 Relay goes down | During P2's call, unplug board 3 | Connection ends `can't be reached (no route)` within ~7 s. Unblock (Tools) and connect again: route `1 > 2, direct` |
| P5 Reroute in flight | Boards 1–4. Block so that 1 reaches 2 through 3 or 4 (block 1↔2 only). Call 1 to 2; Routes shows one middleman (board 3 by the tie-break). Unplug board 3 mid-call | Voice drops for ~2 s at most, then continues via board 4. Serial: `PEER neighbour 3 … stopped acknowledging: routed around` |
| P6 Board comes back | After P5, plug board 3 back in | It reappears in `Peer heard:` at once; routes through it return within ~15 s (trusted gradually) |
| P7 Link loss | Connected over the peer mesh. Tools → **Mute peer mesh** on one board | Connection ends within ~7 s; unmute and it can be made again |
| P8 Everything else | Any test from [handset_espmesh_autoroot_integration_test](../handset_espmesh_autoroot_integration_test/README.md#step-by-step-use-and-test) | Unchanged |

### 4. Host tests (PC, no boards)

```sh
sh tests/run_handset_tests.sh       # includes the peer mesh suite
OPENSSL_PREFIX=/usr sh tests/run_handset_tests.sh   # on Linux
```

`tests/peer_mesh_test.cpp` checks the frames (adverts can't be changed or
forged without the network key; malformed and looping routes are refused)
and the routing in a simulated network:

- a line `1 – 3 – 2`;
- the five-board topology above, with a relay going down and coming back;
- a one-way link (not used) and a weak direct link (two strong hops win);
- a frame rerouted mid-flight by a relay whose next board stopped
  acknowledging;
- **300 random five-board topologies**, before and after a random board
  goes down: every connected pair reaches each other **both ways along the
  same boards**, and unconnected pairs have no route.

## Sniffer (optional, a spare board)

`src/sniffer/main.cpp` is the Arduino kit's keyless sniffer
(`espmesh_sniffer.ino`) as a PlatformIO environment. It never transmits and
holds no keys. Two changes: it knows this project's board 3 MAC, and it
names the peer mesh frames instead of `unknown-format`:

| Line | Meaning |
| --- | --- |
| `enc=clear-advert msg=peer-advert:1:#57:links=2:ttl=4` | Board 1's advert number 57, listing 2 neighbours. Clear by design (signed) |
| `enc=aes-gcm msg=peer:1>3>2@1:voice:1>2:#108` | A voice frame from 1 to 2 on the route 1 > 3 > 2, this copy sent to the board at index 1 (board 3) |

```sh
pio run -e sniffer -t upload --upload-port /dev/cu.usbmodem5B420192791
```

Serial at 921600 baud; `help` lists its commands (`boards any`, `hex on`,
...). It is not in `default_envs`, so a plain `pio run` skips it.

## Serial commands (optional)

Everything from `handset_espmesh_autoroot_integration_test` (`help` lists
them), plus:

| Command | Does |
| --- | --- |
| `peer` | Peer mesh counts (sent, relayed, rerouted, delivered, adverts) and sessions |
| `peer routes` | Every board's route and this board's neighbours with quality |
| `peer connect N` | Connect to board N over the peer mesh (`accept`, `decline`, `disconnect`, `hello`, `talk`, hold OK work over it too) |
| `peer mute on` / `off` | Drop everything over the peer mesh |
| `peer block N` / `peer unblock N` | As Tools → Block Board N |

Log lines:

| Line | Meaning |
| --- | --- |
| `PEER mesh: every board a node and router over ESP-NOW \| …` | At boot |
| `PEER routes \| 2: 1 > 3 > 2, via Board 3 \| … \| neighbours: 3 (91%)` | Whenever a route changes |
| `PEER neighbour 3 (Board 3) stopped acknowledging: routed around` | A relay went quiet; a fresh advert went out |
| `PEER SESSION established with 2 \| …` | A peer mesh session is up |
| `PEER LINK …` | Connection state over the peer mesh |

## Troubleshooting

| You see | Means / try |
| --- | --- |
| `PEER routes \| 2: no route` stays | The boards don't hear each other's adverts. Check they're on this build, on the same channel, and not blocked (Routes: `Peer blocked:`) |
| A route through a board you didn't expect | Ties go to fewer hops, then the lower board ID, and a weak direct link loses to two strong hops. `peer routes` shows each neighbour's quality |
| Dot stays grey but a route exists | The session handshake is still running, or presence pings aren't arriving. Wait a few seconds; `peer` shows the sessions |
| Voice breaks up over 3+ hops | Tools → Rate 6 Mbps on **every** board (less airtime per hop) |
| `rerouted` keeps climbing in `peer` | A relay keeps dropping out: move it, or block it |
| Anything else | See [handset_espmesh_autoroot_integration_test](../handset_espmesh_autoroot_integration_test/README.md#troubleshooting) |

## What changed from handset_espmesh_autoroot_integration_test

- `include/peer_frame.h`, `src/peer_frame.cpp` (new): the data envelope
  (marker `0xF4`, with the whole route), the signed link-state advert
  (marker `0xF5`), route splicing and the duplicate filter.
- `include/peer_routing.h` (new, header-only): neighbour quality, the link
  map, two-way links, symmetric Dijkstra routes.
- `src/peer.inc` (new): adverts, route upkeep, sending, relaying,
  rerouting, sessions, presence, connections, Tools settings and serial
  commands. The every-way helpers (`peer_via()`, `send_control_via()`,
  `route_text_via()`, `gone_reason()`) moved here from `mesh.inc`, since
  this is now the last way.
- `src/handset.inc`: `via::peer` and `peer_calls`; voice redundancy (every
  other frame carries the one before) also over the peer mesh.
- `src/ui.inc`: Peer mesh menu row and list, Status and Routes lines, the
  Routes page scrolls, Tools `Block Board N` and `Mute peer mesh`.
- `src/main.cpp`: peer mesh frames recognised by their first byte before
  the ESP-NOW handling; set-up, service, status and commands.
- `include/radio.h`, `src/radio.cpp`: frames down to 22 bytes are accepted
  (an advert with no neighbours yet is shorter than a sealed frame).
- `include/app_config.h`: the `peer_*` settings.
- `tests/peer_mesh_test.cpp` (new), in `tests/run_handset_tests.sh`.
