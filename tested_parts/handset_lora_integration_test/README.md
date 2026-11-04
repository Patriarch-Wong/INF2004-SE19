# Handset LoRa integration: ESP-NOW voice plus LoRa hello/bye

The whole of [handset_integration_test](../handset_integration_test/), with
LoRa added on the Core1121. Everything ESP-NOW is unchanged: connect,
accept or decline, hold OK to talk, RIGHT says hello, CANCEL disconnects.
That project's README covers it, with hardware tests H1–H17.

LoRa adds a second kind of connection, which carries **only two pre-built
texts**:

1. **Pick LoRa on the menu, then a board.** The other board shows
   **"Board 1 wants to connect over LoRa"** and beeps. Its user accepts
   (OK) or declines (CANCEL).
2. **Send a text.** While connected over LoRa, **UP** sends
   **"Hello from Board 1"** and **DOWN** sends **"Bye from Board 1"**. The
   other board shows it in the banner with a beep, and under
   `Received` on its LoRa screen, with the signal strength. The sender
   shows **Sending**, then **Delivered** once the other board's receipt
   comes back, or **Not delivered** (with a low beep) after 3 tries. One
   text per second. Nothing else is sent over LoRa: no voice, no other
   text.
3. **Disconnect.** CANCEL on either board ends the LoRa connection on both.
   If the other board goes quiet for 20 s (off, or out of range), both
   sides drop it.

**One connection at a time, over either radio.** While a LoRa connection
is asking, asked or connected, an ESP-NOW request gets "Busy", and the
other way round.

Status: **hardware-tested on boards 1 and 2 (2026-11-04): L1–L9 and the
edge cases E1–E10 below pass (34/34 scripted checks), plus a 2-minute
connection kept up by heartbeats alone.** All five environments build and
the host tests pass. ESP-NOW + LoRa together (X1–X9) also pass, apart
from occasional late voice frames (C9, also without LoRa).
Evidence: `evidence/loraA_*`, `evidence/edge1_*`, `evidence/edge2_*`.

**Polish since then (2026-11-04):** delivery receipts for texts, the
repeated accept, staggered first hellos and per-boot random delays (see
[Polish](#polish-after-the-hardware-run)). Run `edge4` on hardware:
**44/44 pass, E1–E11** (`evidence/edge4_*`). It found the hello gap
below, which is built but not yet flashed.

## Why the earlier LoRa build froze, and what's different

The earlier attempt (deleted, never committed) froze on board 1. It
started the radio only when LoRa was picked. The radio failed with
`LR1121 init failed, RadioLib code: -705` (no answer on SPI in time), and
the firmware stopped before drawing the next screen. Board 2 reached its
LoRa menu, so connecting was never tried between two working radios.

This build:

- **Starts the radio in its own task at boot, after the menu is drawn.** A
  radio that never answers can't stop the screen, buttons or ESP-NOW.
- **Never stops on a radio error.** The menu's LoRa dot turns red, the LoRa
  list and Status page show the error code, and the radio is retried every
  5 s.
- **Keeps LoRa on the SPI bus and settings that work** in
  [lora_encrypt_test](../lora_encrypt_test/). The LCD moves to HSPI. The
  earlier build had them the other way round. Set `lora_on_hspi = true` in
  `include/app_config.h` to try that layout.
- **Logs the radio's BUSY pin at each start** (`LORA ok | ... BUSY low N ms
  after reset`, or `BUSY still high`). That tells wiring or power apart from
  an SPI problem.

[lcd_lora_test](../lcd_lora_test/) runs only the LCD and LoRa together. It
is a quicker check if `-705` comes back.

## Fixed on hardware (2026-11-04)

- **A slow radio rebooted the board.** RadioLib waits for BUSY in a loop
  that calls Arduino's `yield()`, which never lets core 0's idle task run
  under the LoRa task, so after 5 s the task watchdog reset the board
  (board 1, every ~8 s). `lora_radio.cpp` gives RadioLib a HAL whose
  `yield()` sleeps 1 ms. Board 1's radio itself turned out to be a
  hardware fault (`-705` in `lora_encrypt_test` too), since fixed.
- **Hellos collided, so the LoRa session took ~12 s.** LoRa is half
  duplex, a frame is ~90 ms on air, and both boards retried every 3 s. The
  lower ID ignores the other's hello and answered "at once", landing on
  the other board's next frame every round. Every LoRa retry (hellos,
  request resends, heartbeats) now gets a random 0–1 s extra delay, and a
  hello is answered 0.3–1.3 s later. Sessions now form 1–2 s after the
  later radio starts.

## Polish after the hardware run

Passed on hardware in `edge4` (2026-11-04), except the hello gap (last
item), which waits for a flash:

- **Texts are confirmed.** A text sent while the other board was sending
  (a heartbeat, its own text) was lost on air and the sender still showed
  `Sent`. Each text is now numbered; the receiver answers every copy with
  an encrypted receipt and shows each number once; the sender tries up to
  3 times, 1.5–2.5 s apart, and shows `Delivered` or `Not delivered`
  (`include/lora_text.h`, host tests `tests/lora_text_test.cpp`). A board
  not connected sends no receipt, so the sender correctly says
  `Not delivered`.
- **A lost accept costs ~1 s, not 2–3 s.** The accept crossed the asker's
  resend on air now and then (seen in the edge runs). The accepting board
  now sends its accept once more 0.4–1.4 s later, unless it has already
  heard the asker (`call::timing.accept_repeat_ms`, 0 for ESP-NOW).
- **The random delays were the same every boot.** The connection
  machine's delays came from a generator seeded with the board ID only,
  and boards 1 and 2's sequences were related (xorshift is linear), so
  their heartbeats drifted together. It is now seeded from the hardware
  RNG, and the ID seed is hashed for the host tests.
- **First hellos are staggered.** When the radio comes up, each first
  session hello waits a random 0–1 s, so two boards powered on together
  don't send theirs at the same moment.
- `lora mute rx` drops only received frames, to test lost receipts; a TX
  dropped by a failed `standby` now counts as `failed` in `lora`.
- **A session hello's answer was lost under the next hello** (found in
  `edge4`, E9b took 16.8 s). After a reboot, board 2 sent hellos to board 1
  and to the absent board 3 0.1–0.3 s apart. Board 1 answers at once, so
  its answer arrived while board 2 was sending the second hello, and was
  never heard; the session formed only at the 5th try. Now no hello
  follows another within `lora_hello_gap_ms` (600 ms). Not yet on
  hardware.

## Screens and buttons

```
Menu ── ESP-NOW ──▶ ESP-NOW list ── OK on Board 2 ──▶ (ESP-NOW as before)
 │                   (dot: online)
 ├──── LoRa ─────▶ LoRa list ──── OK on Board 2 ──▶ Connecting ──accepted──▶ LoRa connected
 │   (dot: radio     (dot: LoRa                      "over LoRa"             UP: Hello
 │    green/red)      link made)                        │                    DOWN: Bye
 ├──── Volume                                           │ declined /         CANCEL: disconnect
 └──── Status                                           ▼ no answer (30 s)      │
                                                    Disconnected ◀───────────────┘
                                                    (back to the list after 3 s)

A request over either radio appears on any screen: OK accepts, CANCEL declines.
```

| Screen | Shows | Buttons |
| --- | --- | --- |
| Menu | `ESP-NOW`, `LoRa` (dot: green radio ok, red failed, grey starting), `Volume`, `Status` | UP/LEFT, DOWN/RIGHT choose; OK selects |
| ESP-NOW list | `Connect Board N` per handset, green dot = online | OK connects; CANCEL back |
| LoRa list | `Board N` per handset, green dot = it answered a LoRa handshake since boot; second hint line: the radio error if it failed | OK connects over LoRa; CANCEL back |
| Connecting | Asking Board 2, **over LoRa** or **over ESP-NOW**, time waiting | CANCEL stops asking |
| Request | Who wants to connect, **over which radio**, "verified by pair key" | OK accepts, CANCEL declines |
| LoRa connected | Peer, connection time, `UP Hello` / `DOWN Bye` (after a press: `Sending`, then `Delivered` in green or `Not delivered` in red), last text received and how long ago, RSSI and SNR | UP sends Hello, DOWN sends Bye, CANCEL disconnects |
| Disconnected | Peer, reason, which radio | Any button, or 3 s, back |
| Status | As before, plus `LoRa ok 915.0 MHz SF7` (or the error), LoRa keys per board, LoRa frames sent and heard | CANCEL back |

The menu now starts with the radio choice, so ESP-NOW connections are one
step deeper (Menu → ESP-NOW → Board 2) than in `handset_integration_test`.

## How LoRa works

- **Encryption:** the same pre-shared pair keys as ESP-NOW, but a **LoRa
  session key of its own** per board, made by the same handshake
  (`session.h`) over LoRa. So it works out of ESP-NOW range, and a frame
  recorded on one radio doesn't open on the other. Connection messages and
  both texts are AES-GCM sealed end to end, with the network-key hop tag,
  exactly like ESP-NOW frames.
- **Sessions:** when the radio comes up, each board sends handshake hellos
  to every other board, every 3 s for 5 tries, then every 30 s. A board
  that boots sends its own hellos, so it's found within a few seconds.
  Picking a board without a LoRa session yet says `no LoRa link yet` and
  asks again at once.
- **Connections:** the same state machine as ESP-NOW (`call.h`), with
  slower timings because each frame is about 80–130 ms on air and the radio
  can't hear while it sends. Requests every 2 s, give up after 30 s.
  Heartbeats every 5 s, link lost after 20 s of silence. If the connected
  board reboots (a new LoRa session), the connection ends at once.
- **Texts:** numbered, answered with a receipt, tried up to 3 times
  (above). Payload: kind (1 text, 2 receipt), number, then the text.
- **No presence pings over LoRa** (airtime). A green LoRa dot only means
  that board answered since boot; asking a board that has since gone off
  ends with `No answer` after 30 s.
- **No relay yet.** Frames for another board are rejected. The design
  review's one-hop relay needs a third board.

Radio settings, the same as `lora_encrypt_test`: **915 MHz**, 125 kHz,
SF7, CR 4/5, sync word 0x12, 8-symbol preamble, CRC on, 0 dBm on the HP
path. They are in `include/app_config.h`; every board must match.

## Wiring

As in [handset_integration_test](../handset_integration_test/README.md#wiring)
(mic GPIO1, speaker GPIO8, LCD, D-pad), plus the Core1121 on the design-review
pins, which none of the other parts use:

| Core1121 | ESP32-S3 |
| --- | --- |
| MISO / MOSI / SCK | GPIO5 / GPIO6 / GPIO7 |
| NSS (CS) / RESET | GPIO15 / GPIO16 |
| BUSY / DIO9 (IRQ) | GPIO17 / GPIO18 |
| VCC / GND | 3V3 / GND |

Attach the antenna before running. The LoRa radio uses the default SPI bus
(SPI2) and the LCD uses HSPI (SPI3), each on its own pins.

## Build and flash

You flash. From this folder, hold BOOT, tap RESET, release BOOT before each
upload, then tap RESET:

```sh
~/.platformio/penv/bin/pio run -e board1 -t upload --upload-port /dev/cu.usbmodem5B5F0216001
~/.platformio/penv/bin/pio run -e board2 -t upload --upload-port /dev/cu.usbmodem5B5F0215971
```

Serial log at 921600 baud:

```sh
~/.platformio/penv/bin/pio device monitor --port /dev/cu.usbmodem5B5F0216001
```

Host tests:

```sh
sh tests/run_host_tests.sh      # crypto and sessions (OpenSSL 3)
sh tests/run_handset_tests.sh   # connections, voice link, DSP, key file
```

## Serial commands

Everything from `handset_integration_test` (`help` lists them), plus:

| Command | Does |
| --- | --- |
| `lora` | Radio state, LoRa link, frame counters, LoRa sessions with RSSI |
| `lora connect N` | Connect to board N over LoRa |
| `lora hello` / `lora bye` | Send the text while connected over LoRa |
| `accept`, `decline`, `disconnect` | Act on the LoRa connection while one is in progress, else on ESP-NOW |
| `press NAME [MS]`, `hold NAME`, `release [NAME]`, `ui` | The D-pad over serial, as before (same handler as the buttons, every screen); `ui` also shows the LoRa texts |
| `lora log on\|off` | One line per LoRa frame sent or received (`LORA TX -> 2 ...`, `LORA RX <- 2 to 1 ... RSSI SNR`); on by default |
| `lora mute on\|off` | Drop every LoRa frame sent and received, like out of range (radio stays on), for link-loss tests |
| `lora mute rx` | Drop only received LoRa frames (lost receipts); `lora mute off` ends it |

Log lines start with `LORA` (radio, sessions, `LORA LINK`, `LORA TEXT`,
`LORA REJECT`). Texts log `LORA TEXT sent ... #N try K of 3`, then
`LORA TEXT delivered ... after K tries` or `LORA TEXT not delivered`; the
receiver logs `LORA TEXT #N from 1 again: receipt resent` for a repeat. Every 10 s the board prints a `LORA |` status line.

## Hardware tests (boards 1 and 2)

| Test | Steps | Pass |
| --- | --- | --- |
| L1 Boot | Reset both boards | Menu draws; log `LORA ok ... BUSY low N ms`; LoRa dot green. If the radio fails: dot red, menu and ESP-NOW still work, error on the LoRa list and Status page, retried every 5 s |
| L2 LoRa sessions | Wait 10 s after both boot, open LoRa | Both lists show the other board green; `lora` shows a session with it |
| L3 Connect | Board 1: LoRa → Board 2, OK. Board 2: OK | Board 2 shows "wants to connect over LoRa"; both reach the LoRa screen |
| L4 Hello and Bye | Board 1 UP, then DOWN; then board 2 UP, then DOWN | Each shows "Hello from Board N" then "Bye from Board N" from the other, with banner, beep and RSSI; the sender shows `Delivered` |
| L11 Not delivered | While connected, power off board 2, then UP on board 1 straight away | Board 1 shows `Sending`, then `Not delivered` with a low beep about 4.5–6.5 s later |
| L5 Decline and cancel | Ask and decline; ask and CANCEL while asking; ask and leave unanswered | `Declined` / `You declined`; `Stopped asking` / `Missed request`; `No answer` after 30 s |
| L6 Disconnect | While connected, CANCEL on either board | Both show `Disconnected` within a few seconds |
| L7 Link lost | While connected, power off board 2 | Board 1 shows `Connection lost` within about 25 s |
| L8 Reboot | While connected, reset board 2 | Board 1 ends the connection when board 2's new LoRa session arrives |
| L9 One at a time | Connected over ESP-NOW, run `lora connect 2` on board 1 (and the reverse) | Refused with `in progress`; a request from a third board would get `Busy` (needs board 3) |
| L10 ESP-NOW still works | Run H1–H4, H11 and H17 from `handset_integration_test` (Menu → ESP-NOW first) | Same results as there |

### Edge cases over the serial D-pad

`tools/lora_edge_test.py` drives both boards with `press` and checks
their logs; it reacts to what each board prints, so timing-sensitive
cases (the request guard) work. Opening the ports resets the boards.

```sh
~/.platformio/penv/bin/python tools/lora_edge_test.py edge1 \
    /dev/cu.usbmodem5B5F0216001 /dev/cu.usbmodem5B5F0215971 [TEST ...]
# TEST: session decline cancel guard texts glare busy noanswer loss reboot reconnect delivery
```

| Case | Result (2026-11-04) |
| --- | --- |
| E1 Boot: LoRa session | Pass: 1.0 s and 1.7 s after the later radio started |
| E2 Connect, texts both ways, reconnect after all below | Pass; board 2 asking works too |
| E3 Second text within 1 s | Pass: ignored, nothing sent (silent, as RIGHT over ESP-NOW) |
| E4 Decline / cancel while asking / no answer | Pass: `declined by peer`; `peer stopped asking`; `no answer` at 30 s and the request screen ends on board 2 |
| E5 2 minutes connected, heartbeats only | Pass: pings 5.1–5.9 s apart, no drop (`loraA`) |
| E6 One connection at a time | Pass: ESP-NOW connect refused during LoRa, LoRa connect refused during ESP-NOW (both boards). `Busy` from a third board needs board 3 |
| E7 Both ask at once | Pass: first requests collided on air; 2.7 s later board 2 (higher ID) gave way and showed board 1's request |
| E8 OK within 500 ms of a request | Pass: ignored; a later OK accepts |
| E9 Link loss / reboot | Pass: `lora mute on` → both `link lost` at 20.0 s; reboot → board 1 ends the connection 6.6 s later on the new LoRa session, and a session is made again. `edge4`: 16.8 s (answers lost under board 2's next hello; fixed by the hello gap, to re-run) |
| E10 Texts outside a connection | Pass: `lora hello` and UP/DOWN on the LoRa list send nothing |
| E11 Delivery receipts (`delivery`) | Pass (`edge4`, 11/11). Delivered on the first try; board 2 muted: 3 tries then not delivered; board 1 `mute rx`: board 2 shows the text once and answers all 3 tries; delivered again after unmuting; the connection holds |

### ESP-NOW and LoRa together (same script)

TEST names: `esp espedge switch crossglare outofrange esploss mutevoice
anywhere espreboot`. Runs `mix1`–`mix3` (2026-11-04).

| Case | Result |
| --- | --- |
| X1 ESP-NOW in this build | Pass: D-pad connect, RIGHT hello (nothing over LoRa), `test 5` both ways 250/250 intact, disconnect |
| X2 ESP-NOW decline, early-OK guard, cancel, PTT collision | Pass: both `hold ok` → one `TX_YIELD`, winner heard intact |
| X3 Switch radios back to back (ESP-NOW, LoRa, ESP-NOW, LoRa) | Pass: each connects and disconnects |
| X4 Board 1 asks over ESP-NOW while board 2 asks over LoRa | Pass, no deadlock: board 1's ESP-NOW request gets `peer busy`, board 2's LoRa request goes on and shows on board 1 |
| X5 ESP-NOW out of range (`radio 1 1` on board 2) | Pass: board 2 offline on ESP-NOW, `connect 2` refused; LoRa connects and Hello arrives; ESP-NOW back online without touching the LoRa connection; ESP-NOW still refused during it |
| X6 ESP-NOW link lost | Pass: dropped after 2.0 s; LoRa connects right after on the same session |
| X7 Voice with LoRa silent vs live (16 × 10 s streams) | 13/16 intact. Late frames: muted `[0,0,0,0,0,0,7,0]`, live `[0,0,1,0,0,0,4,0]`. Happens with LoRa silent too: this is C9 from `handset_integration_test`, not LoRa. One underrun per stream (C8) |
| X8 A request over either radio on any screen | Pass: LoRa request on the ESP-NOW list, ESP-NOW request on the LoRa list |
| X9 Reboot during an ESP-NOW connection | Pass: ESP-NOW ends, both sessions come back, then LoRa and ESP-NOW connect |

Seen but not failures: an accept can be lost when it crosses the asker's
resend (the next resend recovered it, ~2 s later; now the repeated accept
should, after ~1 s); board 1 logs one LoRa receive error per boot during
the first hello burst (the staggered first hellos may remove it; check
`errors` in the `LORA |` line).
