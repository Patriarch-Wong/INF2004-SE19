# Handset ESP-NOW flood integration: four things on the menu

A copy of [handset_broadcast_integration_test](../handset_broadcast_integration_test/)
cut down to **the ESP-NOW flood mesh, LoRa, Broadcast and LoRa broadcast**,
plus Status and Tools. Two changes to how the flood mesh relays are below.

| Menu row | What it does | Reaches a board out of range |
| --- | --- | --- |
| **ESP-NOW flood** | Pick a board, it accepts; hold OK to talk (Opus), RIGHT hello, UP/DOWN volume | yes: every board relays |
| **LoRa** | Pick a board, it accepts; UP "Hello from Board N", DOWN "Bye from Board N", with receipts | no |
| **Broadcast** | No connection: hold OK to talk to all, RIGHT "Hello all" | no: not relayed |
| **LoRa broadcast** | No connection: UP "Hello all", DOWN "Bye all" | no |
| **Status** | IDs, sessions, who is online over the flood mesh, LoRa radio, counters | |
| **Tools** | `Block Board N`, ESP-NOW rate, `Mute flood`, `Mute LoRa` | |

**Status: all five environments build, host tests pass (2026-11-10). Not
tested on hardware: the flood mesh never has been, in any project.**

## What was cut

| Gone from the menu | Still there? |
| --- | --- |
| ESP-NOW (direct connection) | Serial `connect N` still makes one. The pair sessions under it stay: they carry the broadcast keys |
| ESP-MESH | Compiled in, never started: no mesh on the air, no `mesh` commands |
| Routed relay | Compiled in, never set up; any routed frame from another build is dropped |
| Volume page | UP/DOWN on the connected and Broadcast screens still set the volume. Mute is serial `m` only |
| Routes page | The route shows on the connected screen (`Route: 1 > 3 > 2, via Board 3`) |
| Tools: leave/rejoin mesh, be mesh root, mute mesh, mute routed | |

The cut is in `load_rows()` (`src/ui.inc`), `setup()` and `loop()`
(`src/main.cpp`) and `relay_nets` (`src/relay.inc`). The unused screens and
transports were left in the source, not deleted.

## How the flood mesh works

1. Board 1 seals a frame for board 2 under their flood session key, puts
   it in an **envelope** and sends it as an ESP-NOW **broadcast**. The
   envelope holds a hop limit (4 transmissions, `relay_max_hops`) and the
   path so far (`1`).
2. Every board that hears it checks the frame's **hop tag** (HMAC under the
   network key), drops it if it has seen that frame in the last 5 s or is
   already on its path, and otherwise broadcasts it again with itself
   added to the path and one hop fewer.
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

Considered and left out: a board cancelling its relay once it hears
another board relay the same frame. It halves the airtime when every board
hears every other, but a board that is the only way to the destination
would also cancel, and frames would go missing exactly when a relay is
needed.

## Build and flash

Wiring is unchanged
([handset_lora_integration_test](../handset_lora_integration_test/README.md#wiring)).
Every board must run this build: the flood envelope is the same on the
wire, but a board on an older build tells copies apart the old way.

```powershell
cd tested_parts/handset_espnow_flood_integration_test
pio run                      # all five boards
pio run -e board1 -t upload  # hold BOOT, tap RESET first; then board2, ...
```

The board table in `include/network_config.h` is the one from
`handset_peer_mesh_integration_test` (board 3 is `E0:72:A1:D8:F6:20`, board
5 is set), not the copied project's.

## Test cases

None of these needs a board; nothing is uploaded.

```sh
sh tests/run_all_tests.sh        # all of them, about 2 minutes
sh tests/run_build_tests.sh      # T1-T4; add board1 ... to build fewer
sh tests/run_host_tests.sh       # T7
sh tests/run_handset_tests.sh    # T5, T6, T8
```

| ID | Test case | Passes when |
| --- | --- | --- |
| T1 | Every board's firmware compiles (`board1`–`board5`) | `pio run -e boardN` succeeds and `firmware.bin` is made |
| T2 | No compiler warnings from the project | No `warning` line names a file in `src/` or `include/` |
| T3 | Each board gets its own firmware | No two `firmware.bin` files are the same |
| T4 | The PC and test scripts are valid | `tools/*.py` compile, `tests/*.sh` parse |
| T5 | The menu is cut down (`tests/cutdown_test.py`, reads the source) | Menu rows are ESP-NOW flood, LoRa, Broadcast, LoRa broadcast, Status, Tools; Tools has the rate, Mute flood, Mute LoRa; ESP-MESH and the routed relay are not started |
| T6 | The settings are sane (`tests/config_test.cpp`) | Board IDs in order, no MAC twice, names fit the screen, the largest voice frame fits a flood envelope |
| T7 | Flood duplicate filter (`tests/relay_frame_test.cpp`) | A copy is dropped; a frame replayed under the next ID doesn't block the real one |
| T8 | The inherited host suites: crypto, sessions, connections, LoRa texts, voice link, audio DSP, key file | Each prints `All ... passed` |

Last run 2026-11-10: all pass.

Testing on the boards is a separate step, after flashing:
`tools/relay_test.py` and `tools/broadcast_test.py` drive boards 1 and 2
over serial (the docstring of each has the command). `tools/lora_edge_test.py`
is unchanged from the copied project and opens the ESP-NOW menu row, which
this build doesn't have, so its ESP-NOW and mixed cases won't run here.

## Serial commands (optional)

`help` lists them. For the flood mesh: `flood`, `flood connect N`,
`flood mute on|off`, `relay block N`, `relay unblock N`. `accept`,
`decline`, `disconnect`, `hello`, `talk`, `test` and `stop` act on the
connection in progress. The `flood` line counts frames sent, relayed,
dropped as copies and delivered.

Broadcast, LoRa and the log lines are as in
[handset_broadcast_integration_test](../handset_broadcast_integration_test/README.md#serial-commands-optional).

## What changed from handset_broadcast_integration_test

- `src/ui.inc`: the menu and Tools rows listed above; the Status page
  without the ESP-MESH and routed lines; the flood row is labelled
  `ESP-NOW flood`.
- `src/main.cpp`: ESP-MESH is not started, serviced or given commands; the
  `help` text.
- `src/relay.inc`: only the flood mesh is set up and serviced; relays wait
  a random time (`relay_hold()`, `service_relay_queue()`); copies are told
  by the hop tag; a board no longer records its own frames in the
  duplicate filter (it already drops its own echoes by their source).
- `include/relay_frame.h`, `src/relay_frame.cpp`: `frame_key()`; the
  duplicate filter holds 32-bit keys.
- `include/app_config.h`: `relay_jitter_ms`, `relay_queue_depth`, the
  project name.
- `include/network_config.h`: the board table of
  `handset_peer_mesh_integration_test`.
- `tests/relay_frame_test.cpp` (new), in `tests/run_host_tests.sh`;
  `tests/config_test.cpp` (new), in `tests/run_handset_tests.sh`.
- `tests/run_build_tests.sh` and `tests/run_all_tests.sh` (new).
- `tests/cutdown_test.py` (new), in `tests/run_handset_tests.sh`.
- `tools/relay_test.py`: flood only. `tools/mesh_test.py` removed.
