# Handset broadcast integration: talk to every board at once

A copy of [handset_espnow_mesh_integration_test](../handset_espnow_mesh_integration_test/)
that keeps every **peer-to-peer** function (ESP-NOW, ESP-MESH, flood mesh,
routed relay, LoRa) and adds **broadcast**: one board sends, and every board
in range gets it at once. No connection, nothing to accept, no router.

| | Peer to peer (unchanged) | **Broadcast (new)** |
| --- | --- | --- |
| Who gets it | the one board you connected to | **every board in range** |
| Setting up | pick a board, it accepts | none: open the page and talk |
| ESP-NOW | voice (hold OK), hello (RIGHT) | **voice to all (hold OK), "Hello all from Board N" (RIGHT)** |
| LoRa | "Hello / Bye from Board N", with delivery receipts | **"Hello all / Bye all from Board N", no receipts** |
| On the radio | one frame to that board | **one frame to everyone**: ESP-NOW to `FF:FF:FF:FF:FF:FF`; LoRa frames are heard by all anyway |
| Encrypted | pair session key | **the sender's broadcast key** (below) |

**Status: all five environments build. Not yet tested on hardware.**
`tools/broadcast_test.py` is ready for the first run (step 7).

## How broadcast works

### On the radio

- **ESP-NOW broadcast** sends each frame once to the broadcast address.
  Every board in range receives the same frame. No board relays it: it
  reaches only boards in direct range. For relaying to far boards, use the
  flood mesh or routed relay pages, which are peer to peer.
- **Voice** is the same Opus push-to-talk stream as peer to peer:
  20 ms frames, each carrying the previous one as well (redundancy). Every
  listener asks for frames it missed (NACK), and the talker resends them
  to everyone.
- **LoRa broadcast** is one LoRa frame with "Hello all from Board N" or
  "Bye all from Board N". Peer-to-peer LoRa texts get a receipt from the
  other board; a broadcast can't, because several boards answering at once
  would collide on air. If a board misses it, it misses it.
- **Broadcast frames are not acknowledged.** Peer-to-peer ESP-NOW frames
  get a radio-level acknowledgement and retries; broadcast frames don't.
  That is why the ESP-NOW rate stays at the default **1 Mbps**: an earlier
  broadcast test (`e2e_broadcast_test`) lost frames at 6 Mbps and passed at
  1 Mbps.

### Keys: how a broadcast stays end-to-end encrypted

A pair session key is shared by only two boards, so it can't seal a
message for everyone. Instead every board uses **sender keys**:

1. At every boot, each board makes its own random **broadcast key**: one
   for ESP-NOW and one for LoRa.
2. It gives that key to every paired board over their **pair session** on
   that radio. That message is sealed, so only that board can read it, and
   only the real sender can have sent it.
3. The other board answers with a **receipt**. Until then the key is sent
   again every 1 s (ESP-NOW) or every 4–5 s (LoRa), and again after any new
   session, for example after a reboot.
4. A broadcast is sealed **once** under the sender's key, addressed to group
   ID 255, and opens on every board that holds that key.

**Replay protection.** The key comes with the sender's current counter. A
frame from before the key arrived, or a frame seen before, is refused.
Recorded broadcasts therefore don't play again, even after a reboot: a
board that reboots makes a new key, and a board that receives after
rebooting gets the counter again.

**What the key proves.** Every paired board holds the sender's broadcast
key, and it is shared, not a signature. A broadcast therefore proves "from
a board of this group", not which board. A board outside the group can't
read or forge one.

### One connection at a time

**A board in a connection takes no broadcasts**, so a private call is never
talked over. It logs that it ignored one. You also can't broadcast from a
connection. The Broadcast pages are on the menu, which isn't shown while a
connection is in progress.

## Screens and buttons

Menu: ESP-NOW, ESP-MESH, Flood mesh, Routed relay, LoRa, **Broadcast**,
**LoRa broadcast**, Volume, Status, Routes, Tools. The menu scrolls: 4 rows
show at a time.

**Broadcast** (ESP-NOW voice and hello to all):

```
+------------------------------------+
| Broadcast                  Board 1 |
| To all boards                      |
| ---------------------------------- |
| Heard by: 2 ok 3 -                 |  <- who has this board's key
| +--------------------------------+ |
| |        Hold OK to talk         | |  <- TALKING / "Board 2 is talking"
| +--------------------------------+ |
| Vol 16  [==========          ]     |
| Last heard 4 s ago                 |
| 150/150 frames ok                  |
| Hold OK: talk to all  UP/DOWN: vol |
| RIGHT: hello to all   CANCEL: back |
+------------------------------------+
```

| Button | Does |
| --- | --- |
| hold **OK** (or PTT) | Talk to every board; release to stop |
| **RIGHT** | "Hello all from Board N" to every board (banner on theirs) |
| **UP** / **DOWN** | Volume |
| **CANCEL** | Back to the menu |

- `Heard by: 2 ok 3 -` lists the boards that hold this board's key and are
  online: board 2 will hear you, board 3 not yet (off, out of range, or
  still getting the key). It turns green once any board is `ok`.
- **When another board broadcasts, your screen jumps to the Broadcast page
  by itself.** The box shows `Board 2 is talking`, so you can answer at
  once.

**LoRa broadcast**:

| Button | Does |
| --- | --- |
| **UP** | "Hello all from Board N" over LoRa to every board |
| **DOWN** | "Bye all from Board N" |
| **CANCEL** | Back to the menu |

- The box shows `Sent to all` after a press. It means sent, not heard:
  there are no receipts.
- Below the box: the last broadcast received (who sent it, how long ago,
  the text) and the last one this board sent.
- `Heard by:` works as on the Broadcast page, for the LoRa key. LoRa has no
  presence pings, so `ok` there means "has the key", whether or not that
  board is on right now.

Every other screen is as in
[handset_espnow_mesh_integration_test](../handset_espnow_mesh_integration_test/README.md#screens-and-buttons).

## Step-by-step: use and test

You need boards 1 and 2 with the handset wiring. Board 3 is optional; it
shows that broadcasts really reach everyone. Wiring is unchanged
([handset_lora_integration_test](../handset_lora_integration_test/README.md#wiring)).
Every step works from the D-pad and the screen; no serial monitor needed.

### 1. Flash all boards with this build

From this folder. These boards need **BOOT + RESET** before each upload:
hold BOOT, tap RESET, release BOOT, upload, tap RESET. Close serial
monitors first: an open monitor blocks the port.

| USB serial (`pio device list`, `SER=`) | Board | COM on the PC this was written on |
| --- | --- | --- |
| `5B5F021600` | Board 1 | COM11 |
| `5B5F021597` | Board 2 | COM4 |
| `5B5F021589` | Board 3 (MAC `E0:72:A1:D9:A4:94`) | COM10 |
| `5B42019238` | Board 4 (MAC `E0:72:A1:D8:0E:48`) | COM15 |

```powershell
cd tested_parts\handset_broadcast_integration_test
~\.platformio\penv\Scripts\pio.exe run -e board1 -t upload --upload-port COM11
~\.platformio\penv\Scripts\pio.exe run -e board2 -t upload --upload-port COM4
~\.platformio\penv\Scripts\pio.exe run -e board3 -t upload --upload-port COM10
~\.platformio\penv\Scripts\pio.exe run -e board4 -t upload --upload-port COM15
```

Each board needs its own environment (`boardN`), or it stops with "Wrong
boardN environment". **Flash every board from this folder.** A board still
on an older build has no broadcast keys, so it can't hear broadcasts.

### 2. Wait for the keys

Reset all boards (RESET button) at about the same time.

1. On board 1: Menu → DOWN to **Broadcast** → OK.
2. Within about 10 s the line under the hline reads
   **`Heard by: 2 ok 3 ok`** (green); 3 only if board 3 is on. That means
   boards 2 and 3 now hold board 1's ESP-NOW key.
3. CANCEL, Menu → **LoRa broadcast**: the same line for LoRa. LoRa is
   slower: allow up to a minute. It needs the LoRa sessions first, the
   ones that turn the LoRa list's dots green.

What is happening: at boot each board made fresh broadcast keys, made its
pair sessions with the others as usual, then handed its key over each
session and waited for a receipt ("How broadcast works" above).

### 3. Say hello to all (ESP-NOW)

1. Board 1, on the **Broadcast** page: press **RIGHT**.
2. Board 1's banner: **`Hello sent to all`**.
3. Boards 2 and 3: a short beep and the banner
   **`Hello all from Board 1`**, whatever screen they are on.

### 4. Talk to all (ESP-NOW)

1. Board 1, on the **Broadcast** page: **hold OK** and speak. The box shows
   **TALKING** in red.
2. Boards 2 and 3 **jump to their Broadcast page** by themselves. Their box
   shows **`Board 1 is talking`** in green, and they play your voice.
3. Release OK. Boards 2 and 3 give a short "roger" beep: your turn is over.
   Their bottom lines show `Last heard 0 s ago` and `N/N frames ok`.
4. Answer from board 2: it is already on the Broadcast page, so hold OK.
   Board 1 (and 3) hear it.

**If two boards press at the same moment**, one keeps the floor and the
other stops and listens, as with peer-to-peer push-to-talk.

### 5. LoRa hello and bye to all

1. Board 1: Menu → **LoRa broadcast** → **UP**. The box shows
   `Sent to all` / `Hello all`.
2. Boards 2 and 3: beep, and the banner **`Hello all from Board 1`**. On
   their own LoRa broadcast page the text appears under
   `From Board 1, N s ago:`.
3. Board 2: **DOWN** on its LoRa broadcast page. Board 1 receives
   `Bye all from Board 2`.

### 6. Check the rules

| Test | Steps | Pass |
| --- | --- | --- |
| T1 Connection first | Board 1 and 2 connected over ESP-NOW (Menu → ESP-NOW → Connect Board 2, accept). Board 3: Broadcast page, RIGHT, then hold OK | Boards 1 and 2 show nothing: a connection takes no broadcasts. Board 3's `Heard by` still says `1 ok 2 ok` (they hold its key, they just ignore it) |
| T2 Back to broadcast | Disconnect boards 1 and 2. Board 3 talks again | Boards 1 and 2 jump to the Broadcast page and hear it |
| T3 Reboot | Board 2: RESET. Watch board 1's Broadcast page | `Heard by: 2 -` for a few seconds, then `2 ok` again: board 2 got board 1's key afresh |
| T4 Out of range | Carry board 2 out of range and talk on board 1 | Board 2 hears nothing; it isn't relayed. Peer to peer, the flood mesh or routed relay pages reach it through board 3 |
| T5 Old build | A board flashed from another folder | Never shows `ok` in `Heard by`, and hears no broadcasts |
| T6 Peer to peer still works | Any test from the source folder's guide | Unchanged |

### 7. Optional: scripted test from a PC

You don't need this; it repeats the steps above automatically.
`tools/broadcast_test.py` drives boards 1 and 2 over USB serial and checks
their logs. Close the serial monitors first; the script reboots both
boards. Port 1 must be board 1.

```powershell
~\.platformio\penv\Scripts\python.exe tools\broadcast_test.py bcast1 COM11 COM4
```

| Test | What it checks |
| --- | --- |
| B1 `keys` | Each board took the other's broadcast key, over ESP-NOW and over LoRa |
| B2 `hello` | `bcast hello` on board 1: board 2 gets "Hello all from Board 1" and shows it in the banner |
| B3 `voice` | 5 s test signal from board 1 to all: board 2 plays every frame intact and jumps to the Broadcast page |
| B4 `reply` | The same from board 2 to all |
| B5 `dpad` | The same from the D-pad: menu → Broadcast, RIGHT, hold OK 3 s |
| B6 `lora` | LoRa hello to all from board 1, bye to all from board 2 |
| B7 `busy` | While boards 1 and 2 are connected peer to peer, broadcasting is refused |
| B8 `reboot` | Board 2 reboots; board 1's key reaches it again and broadcasts get through |

Logs land in `evidence/`. Check the voice streams frame by frame, sender's
log first:

```powershell
~\.platformio\penv\Scripts\python.exe tools\check_streams.py evidence\bcast1_board1.txt evidence\bcast1_board2.txt
```

`tools/relay_test.py` and `tools/mesh_test.py` run on this build too, for
the peer-to-peer ways.

## Serial commands (optional)

Nothing above needs them; they are the same functions, for scripted tests
and for reading the log. Everything from
`handset_espnow_mesh_integration_test` (`help` lists them), plus:

| Command | Does |
| --- | --- |
| `bcast` | Both broadcast keys, counters, and for each board: has it got this board's key, and this board's copy of its key |
| `bcast hello` | Broadcast page → RIGHT |
| `bcast talk [S]` / `bcast test [S]` | Talk to all (microphone, or the test signal) for S seconds; `stop` ends it |
| `lora bcast hello` / `lora bcast bye` | LoRa broadcast page → UP / DOWN |

Log lines:

| Line | Meaning |
| --- | --- |
| `BCAST new broadcast keys for this boot: …` | At boot |
| `BCAST key offered to N` / `BCAST key delivered to N` | This board's key going to board N / its receipt |
| `BCAST key from N (…): ID, its broadcasts from counter C` | Board N's key arrived |
| `BCAST TEXT sent to all: "…" \| boards with this board's key: 2 3` | A hello went out |
| `BCAST TEXT from N` | A hello came in |
| `TX_START … peer=255 … via=broadcast`, `RX_START … via=broadcast` | Voice to or from all |
| `BCAST from N ignored: a connection is in progress` | The connection rule, at most every 2 s |
| `BCAST REJECT … no broadcast key from this board yet` | A broadcast before its key arrived (at most every 2 s per board) |
| `LORA BCAST …` | The same for LoRa |

## Troubleshooting

| You see | Means / try |
| --- | --- |
| `Heard by: 2 -` stays | Board 2 isn't on this build, is off, or has no session with this board yet: is it green on the ESP-NOW list? For LoRa: is it green on the LoRa list? LoRa keys retry every 4–5 s, then every 30 s after 5 tries |
| Broadcast voice `MISSING` frames | Keep the rate at 1 Mbps (Tools → Rate shows it); 6 Mbps loses broadcast frames. Bring the boards closer: broadcasts aren't relayed |
| A board doesn't hear broadcasts | It's in a connection (disconnect), out of direct range, or on an older build |
| `BCAST REJECT … no broadcast key from this board yet` in a log | The sender's key hadn't arrived yet; it does within seconds of the session |
| Screen jumped to Broadcast by itself | Another board started talking to all. CANCEL goes back |
| `Stopped` with `Wrong boardN environment` or `This MAC is not in network_config.h` | See [handset_espmesh_integration_test](../handset_espmesh_integration_test/README.md#troubleshooting) |

## What changed from handset_espnow_mesh_integration_test

- `src/broadcast.inc` (new):
  - broadcast keys: made at boot, handed over and receipted over the pair
    sessions;
  - sealing and opening broadcasts;
  - ESP-NOW broadcast send and receive;
  - LoRa broadcast texts;
  - the `bcast` commands.
- `src/handset.inc`:
  - `via::broadcast`, a way with no connection: voice and text only;
  - `start_talk_to()`, so the same push-to-talk code talks to one board or
    to all;
  - `takes_from()` / `same_party()`, for who voice and text are taken from
    (over a broadcast: anyone, while not in a connection);
  - broadcast frames and key messages routed to `broadcast.inc`.
- `src/lora.inc`: LoRa broadcasts and LoRa key messages routed to
  `broadcast.inc`.
- `src/ui.inc`:
  - the **Broadcast** and **LoRa broadcast** pages and their menu rows;
  - the jump to the Broadcast page when another board talks to all;
  - the listener's name in the talk box comes from the stream, the same
    board as before over a connection.
- `include/network_config.h`: `broadcast_id` (255).
- `include/app_config.h`: key retry and log timings.
- `tools/broadcast_test.py` (new).

Peer-to-peer behaviour, frames and log lines are unchanged.
