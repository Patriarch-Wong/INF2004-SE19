# Handset ESP-MESH auto-root integration: elected root, no routed relay

A copy of [handset_broadcast_integration_test](../handset_broadcast_integration_test/)
(the newest build) with two changes:

1. **The ESP-NOW flood mesh and routed relay are removed entirely**: no
   `relay.inc`, no relay frames, no menu pages, Tools rows or serial
   commands for them. ESP-MESH is the only mesh.
2. **The ESP-MESH root is elected automatically** instead of the fixed order
   (board 1, then 2, then 3…). An **unused board in the middle** becomes the
   root, so the board that most traffic passes through isn't one of the
   boards talking.

Everything else is unchanged and works as before: ESP-NOW connections,
ESP-MESH connections, LoRa connections, ESP-NOW and LoRa broadcasts, the
D-pad and screen, encryption.

| Way | Topology | Reaches a board out of range |
| --- | --- | --- |
| ESP-NOW | direct links | no |
| **ESP-MESH** | **tree, elected root** | **yes, through any boards between them in the tree** |
| LoRa | direct links | no |
| Broadcast (ESP-NOW / LoRa) | one sender to every board in range | no |

**Status: all five environments build, host tests pass (including the new
`root_election` suite). Not yet tested on hardware.**

## How the root is elected

ESP-MESH (Espressif's ESP-WIFI-MESH stack) normally votes for the board with
the strongest signal to a Wi-Fi router. There is no router here, so it has
nothing to vote on. Instead the boards elect the root among themselves,
over ESP-NOW.

**The score.** Every board's ESP-NOW presence ping (every 2 s, sealed under
the pair session like everything else) now carries its score:

- how many boards it hears directly over ESP-NOW;
- whether it is **busy** (in a connection over any radio);
- whether it is **out of the mesh** (`Leave mesh`, or its mesh failed to
  start).

**The ranking.** Each board ranks every board it hears, itself included:

| Order | Rule | Why |
| --- | --- | --- |
| 1 | Idle beats busy | An unused board relays; boards that are talking don't also carry everyone else's traffic |
| 2 | More boards heard beats fewer | The board in the middle, which the others reach in one hop, makes the tree shallow |
| 3 | Lower ID beats higher | Tie-break: every board orders the same way |

A board out of the mesh is left out entirely, so nobody waits for it. A
board heard but whose score hasn't arrived yet ranks by ID alone, as the
old fixed order did.

**What the rank does.** It only sets how long a board waits before making
itself root. Every board starts as a node and looks for a mesh to join. If
it finds none, or its mesh lost its root, for its rank's wait, it becomes
the root and the others join it:

| Rank | Becomes root after looking for |
| --- | --- |
| 0 (the best board) | 6 s (`mesh_root_wait_ms`) |
| 1 | 14 s (+8 s per place, `mesh_root_wait_step_ms`) |
| 2 | 22 s |

The rank is worked out again every time the wait is checked, so it uses
the latest scores, and a board that has just gone down drops out of the
ranking within ~6.5 s.

**Examples:**

| Boards | Root |
| --- | --- |
| 1, 2, 3 all idle, all in range of each other | Board 1 (tie, lowest ID) |
| 1 – 3 – 2 in a line (1 and 2 out of each other's range) | Board 3 (it hears 2 boards, the ends hear 1) |
| Root goes down while boards 1 and 2 are in a call, board 3 idle | Board 3 (idle beats busy) |
| Root goes down, board 1 has `Leave mesh` on | Board 1 is skipped |

### The relays: any board between

The mesh is a tree. A frame from one board climbs towards the root until it
reaches a board the destination sits under, then goes down to it. **Any
board between the two passes frames on**, whether it has a speaker or not;
ESP-MESH does the forwarding by itself, and the relay can't open the frame
(end-to-end encryption). Which board each one hangs from (its parent) is
chosen by ESP-MESH: a board closer to the root first, then the better
signal, as long as that parent still has room.

The election decides the root only. The root is the board most traffic can
pass through, so that is where being unused matters most.

### What the election doesn't do

- **A working root is never moved for a better one.** Moving the root
  restarts the mesh under every board and drops connections over it. The
  election only runs when there is no root (at power-on, or after the root
  went down), so the root can't flap back and forth.
- **A board that can't hear the others over ESP-NOW** ranks itself first
  and may become a root on its own. That is the existing split case: a root
  that nobody joins for 30–45 s steps down and looks for another mesh to
  join (unchanged from the ESP-MESH build). The same covers the rare case of
  two boards each ranking themselves first before the other's score arrived.

Root failover, the split rule and why restarts never cut voice are as in
[handset_espmesh_integration_test](../handset_espmesh_integration_test/README.md#when-a-board-goes-down),
apart from the order, which is now the election's.

## Screens and buttons

Menu: ESP-NOW, ESP-MESH, LoRa, Broadcast, LoRa broadcast, Volume, Status,
Routes, Tools (9 rows; it scrolls).

| Screen | Changed |
| --- | --- |
| Menu | No Flood mesh or Routed relay rows |
| Status | The old flood/routed line now shows this board's score: `Root: idle, hears 2, rank 0` |
| Routes | Each board's route over ESP-MESH only (`Mesh 1 > 3 > 2, via Board 3`), then the same `Root: …` line |
| Tools | `Rate`, `Leave mesh` / `Rejoin mesh`, `Be mesh root`, `Mute mesh`, `Mute LoRa`. The `Block Board N`, `Mute flood` and `Mute routed` rows are gone |

Every other screen is as in
[handset_broadcast_integration_test](../handset_broadcast_integration_test/README.md#screens-and-buttons).

## Step-by-step: use and test

Boards 1 and 2 with the handset wiring, board 3 for the election and
middleman tests (no speaker needed). Wiring is unchanged
([handset_lora_integration_test](../handset_lora_integration_test/README.md#wiring)).

### 1. Flash all boards with this build

From this folder. These boards need **BOOT + RESET** before each upload:
hold BOOT, tap RESET, release BOOT, upload, tap RESET. Close serial
monitors first.

| USB serial (`pio device list`, `SER=`) | Board | COM on the PC this was written on |
| --- | --- | --- |
| `5B5F021600` | Board 1 | COM11 |
| `5B5F021597` | Board 2 | COM4 |
| `5B5F021589` | Board 3 (MAC `E0:72:A1:D9:A4:94`) | COM10 |

```powershell
cd tested_parts\handset_espmesh_autoroot_integration_test
~\.platformio\penv\Scripts\pio.exe run -e board1 -t upload --upload-port COM11
~\.platformio\penv\Scripts\pio.exe run -e board2 -t upload --upload-port COM4
~\.platformio\penv\Scripts\pio.exe run -e board3 -t upload --upload-port COM10
```

**Flash every board from this folder.** A board on an older build sends no
score, so the others rank it by ID alone, and it still uses the fixed order
itself.

### 2. Check the election

1. Reset all boards at about the same time.
2. On each board: Menu → **Status**. Within a few seconds the score line
   reads `Root: idle, hears 2, rank N`. Exactly one board should show
   **rank 0**: board 1 when all three are in range of each other (tie,
   lowest ID).
3. About 6 s after boot, Menu → **Routes** on that board: `Mesh: root, 3
   boards`. The others show `Mesh: layer 2, parent Board 1`.

### 3. Tests on the boards

| Test | Steps | Pass |
| --- | --- | --- |
| E1 Tie | All three idle and close together; reset all | Board 1 is root (Routes), boards 2 and 3 join it |
| E2 Middle board | Put board 3 between boards 1 and 2, with 1 and 2 out of each other's range (or far apart). Reset all | Status on board 3 shows `hears 2`, the ends `hears 1`; **board 3 becomes root**, and a mesh call between 1 and 2 shows `Route: 1 > 3 > 2, via Board 3` |
| E3 Unused board takes over | After E1 (board 1 root): connect boards 1 and 2 over ESP-NOW, so both are busy. The menu is hidden during a call, so take the root down from serial: `mesh off` on board 1 | Board 3 (idle) becomes root after ~6 s, although board 2 has the lower ID; board 2 joins it. Routes on board 3 shows `Mesh: root` |
| E4 Root comes back | After E3: `mesh on` on board 1 (or disconnect, then Tools → **Rejoin mesh**) | Board 1 joins board 3's mesh as a node; the root doesn't move back |
| E5 Out of the mesh is skipped | Board 1: Tools → **Leave mesh**; on board 2: `Status` | Board 1 isn't counted: board 2's rank drops by one, and nobody waits for board 1 |
| E6 Mesh call through any board | E2's layout, then Menu → ESP-MESH → Connect, hold OK on board 1 | Board 2 hears it through board 3 |
| E7 Everything else | Any test from [handset_broadcast_integration_test](../handset_broadcast_integration_test/README.md#step-by-step-use-and-test) except the flood/routed ones | Unchanged |

### 4. Optional: scripted test from a PC

`tools/mesh_test.py` drives boards 1 and 2 over USB serial (it reboots
both). Port 1 must be board 1. It now includes `elect`: both boards print
their election line, both are idle, each hears the other, board 1 is rank
0 and board 2 rank 1, and while connected over ESP-NOW both report busy.

```powershell
~\.platformio\penv\Scripts\python.exe tools\mesh_test.py auto1 COM11 COM4
~\.platformio\penv\Scripts\python.exe tools\mesh_test.py auto1 COM11 COM4 join elect rootdown
```

`tools/broadcast_test.py` and `tools/lora_edge_test.py` run on this build
too.

## Serial commands (optional)

Everything from `handset_broadcast_integration_test` (`help` lists them)
except `flood`, `routed`, `routes`, `relay block/unblock`, `flood mute`
and `routed mute`. New:

| Command | Does |
| --- | --- |
| `mesh elect` | This board's score and rank, and every board's score as heard |

Log lines:

| Line | Meaning |
| --- | --- |
| `MESH starting \| … \| root: elected (idle first, then most boards heard, then lowest ID)` | At boot |
| `ELECT \| this board 2: idle, hears 2 \| rank 1, root after 14 s with no mesh \| 1: idle, hears 2, ahead \| 3: idle, hears 1 \|` | `mesh elect`, and just before a board makes itself root |
| `ELECT \| … 3: no score yet (ID order)` | Board 3 is heard but on an older build, or its first ping hasn't come |
| `ELECT \| … 1: out of the mesh` | Board 1 has left the mesh (skipped) |
| `MESH no mesh with a root for N s (this board's turn comes after M s): becoming the root` | This board won the election |

## Troubleshooting

| You see | Means / try |
| --- | --- |
| Two boards both show `rank 0` | They can't hear each other over ESP-NOW (check the ESP-NOW list's dots), or one is on an older build. A root nobody joins steps down within 30–45 s |
| The root isn't the board you expected | Check `hears` on Status: the board that hears the most idle boards wins; ties go to the lowest ID. A working root never moves, so reset all boards to elect again |
| `no score yet (ID order)` stays for a board | That board isn't on this build |
| Anything else | See [handset_espmesh_integration_test](../handset_espmesh_integration_test/README.md#troubleshooting) and [handset_broadcast_integration_test](../handset_broadcast_integration_test/README.md#troubleshooting) |

## What changed from handset_broadcast_integration_test

- Removed: `src/relay.inc`, `src/relay_frame.cpp`, `include/relay_frame.h`,
  `tools/relay_test.py`, and every use of them:
  - `src/handset.inc`: `via::flood` / `via::routed`, their connections and
    sends;
  - `src/ui.inc`: the Flood mesh and Routed relay pages, their Status and
    Routes lines, and the `Block`, `Mute flood`, `Mute routed` Tools rows;
  - `src/main.cpp`: relay receive, set-up, service, status and commands;
  - `include/app_config.h`: the `relay_*` settings;
  - `src/radio.cpp`, `include/radio.h`: the shortest frame is a
    `secure_message` frame again (relay route messages were shorter).
- `include/root_election.h` (new): the score, the ranking and the ping
  flags; host-tested by `tests/root_election_test.cpp` (in
  `tests/run_handset_tests.sh`).
- `src/handset.inc`: the ESP-NOW presence ping carries the score
  (`kind_presence`, boards heard, flags); received scores are kept per
  board.
- `src/mesh.inc`: the election replaces the fixed order (`mesh_rank()`),
  `mesh elect`, the score line for the screens. The two-way helpers
  (`peer_via()`, `send_control_via()`, `route_text_via()`, `gone_reason()`)
  are back here from `relay.inc`.
- `include/network_config.h`: `mesh_root_id` removed.
- `tools/mesh_test.py`: the `elect` test; board 1 is root by the tie-break.
