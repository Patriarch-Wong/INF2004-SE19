# SE19 handset: encrypted push-to-talk over ESP-NOW and LoRa

## Purpose and scope

An ESP32-S3 handset for talking and sending short texts with no router,
access point or phone network. The scope of this firmware is:

1. half-duplex push-to-talk voice between two handsets over ESP-NOW,
2. relaying of that traffic by the boards in between, so a call reaches a
   handset out of direct range,
3. short texts over the Core1121 LoRa module,
4. voice and text to every handset at once, over ESP-NOW and LoRa,
5. end-to-end encryption of all of it.

Every board runs the same firmware and is both a handset and a relay.

- [Functions](#functions)
- [Specifications](#specifications)
- [Hardware](#hardware)
- [Firmware design](#firmware-design)
- [Security](#security)
- [Setting up](#setting-up)
- [Using the handset](#using-the-handset)
- [Serial commands](#serial-commands)
- [Configuring for other boards](#configuring-for-other-boards)
- [Tests](#tests)
- [Limitations](#limitations)
- [Troubleshooting](#troubleshooting)
- [Source layout](#source-layout)

## Functions

| ID | Function |
| --- | --- |
| F1 | **Connection.** The user picks another handset from a list. That handset shows who is asking, and its user accepts or declines. A handset holds one connection at a time |
| F2 | **Push-to-talk voice.** While connected, holding OK captures the microphone and sends it; releasing OK stops. One side talks at a time |
| F3 | **Relay.** A frame for a handset out of direct range is passed on by the boards that hear it, through up to three relays. Any board that isn't in a connection of its own relays |
| F4 | **Hello over ESP-NOW.** While connected, RIGHT sends "Hello from Board N" to the other handset |
| F5 | **Text over LoRa.** Two handsets connect over LoRa and send "Hello from Board N" or "Bye from Board N". The sender sees whether each text was delivered |
| F6 | **Broadcast.** With no connection, a handset talks or sends "Hello all" to every handset in direct ESP-NOW range |
| F7 | **LoRa broadcast.** With no connection, a handset sends "Hello all" or "Bye all" to every handset in LoRa range |
| F8 | **Presence.** The lists show which handsets are online |
| F9 | **User interface.** Every function is reached with six buttons and the LCD. A status page shows IDs, links and counters |
| F10 | **Encryption.** Every message is sealed so that only its destination opens it. A relay passes a message on without being able to read it |
| F11 | **Link loss.** A connection ends on both sides when either user disconnects or the link goes quiet |
| F12 | **Serial control.** Every function is also a serial command, for scripted tests |

## Specifications

| | |
| --- | --- |
| Board | ESP32-S3 N16R8 (16 MB Quad-SPI flash, 8 MB Octal-SPI PSRAM) |
| Network size | 6 boards |
| Toolchain | PlatformIO, Arduino framework, `espressif32@7.1.3` (Arduino-ESP32 2.0.17) |
| Libraries | arduino-libopus, Adafruit ST7735 and ST7789 Library 1.11.0, RadioLib 7.7.1 |
| Firmware size | About 1.05 MB, in a 6.4 MB application slot |
| Voice radio | ESP-NOW on 2.4 GHz, channel 6, 1 Mbps, frames up to 250 bytes |
| Relaying | Flood: at most 4 transmissions per frame, so up to 3 relays. A relay waits a random 0–20 ms |
| Voice codec | Opus, 16 kHz mono, 24 kbit/s constant bit rate, 20 ms frames (60-byte packets) |
| Talking | Push-to-talk, half duplex, up to 60 s per press |
| Lost voice frames | Each packet also carries the frame before it; the listener asks again for missing frames; 300 ms playout buffer, 800 ms for a call through a relay (60–1000 ms) |
| Microphone | MAX9814 on ADC1, sampled at 64 kHz and averaged to 16 kHz, 300–3000 Hz band-pass, noise gate |
| Speaker | 8-bit PWM at 78.125 kHz, one duty update per sample at 16 kHz, volume 1–32 (default 3) |
| Text radio | LoRa on a Waveshare Core1121 (LR1121): 915 MHz, 125 kHz bandwidth, SF7, coding rate 4/5, 0 dBm, sync word 0x12 |
| LoRa text | Up to 3 tries 1.5 s apart, until a delivery receipt comes back |
| Display and input | ST7789 240 × 240 SPI LCD, six push buttons, 30 ms debounce |
| Encryption | AES-128-GCM with a 16-byte tag; a new session key every boot (HKDF-SHA256); 64-counter replay window; HMAC-SHA256 hop tag; 40 bytes added to each message |
| Presence | A ping every 2 s; a board shows offline after 6.5 s without one |
| Connections | A request waits 30 s for an answer. A connection is dropped after 6 s of silence (20 s over LoRa) |
| Serial | 921600 baud on the UART connector |

## Hardware

### Parts

For each handset:

| Part | Notes |
| --- | --- |
| ESP32-S3 N16R8 development board | Two or more, up to six |
| MAX9814 microphone module | Analog output |
| Speaker module | Powered, with a signal input |
| ST7789 240 × 240 LCD | Seven-pin SPI module, no CS pin |
| Six push buttons | UP, DOWN, LEFT, RIGHT, OK, CANCEL |
| Waveshare Core1121 LoRa module | With a 915 MHz antenna |
| USB cable | To the board's **UART** connector |

A board used only as a relay needs none of the parts, only power.

### Wiring

Wire with the board unplugged. The pins are set in `include/app_config.h`.

| Module | Module pin | ESP32-S3 | Interface | Owned by |
| --- | --- | --- | --- | --- |
| MAX9814 microphone | VDD / GND | 3V3 / GND | | |
| | OUT | **GPIO1** | ADC1 channel 0 | Mic task |
| | GAIN | 3V3 (AR not connected) | | |
| Speaker module | VCC / GND | 5V / GND | | |
| | SIG | **GPIO8** | PWM | Speaker task |
| ST7789 LCD | VCC / GND | 5V / GND | | |
| | SCL / SDA | GPIO12 / GPIO11 | SPI clock / data | Loop task |
| | RES / DC / BLK | GPIO10 / GPIO9 / GPIO14 | GPIO | Loop task |
| Core1121 LoRa | VCC / GND | 3V3 / GND | | |
| | MISO / MOSI / SCK | GPIO5 / GPIO6 / GPIO7 | SPI, its own bus | LoRa task |
| | NSS (CS) / RESET | GPIO15 / GPIO16 | GPIO | LoRa task |
| | BUSY / DIO9 (IRQ) | GPIO17 / GPIO18 | GPIO / interrupt | LoRa task |
| Button UP | 3V3 → | GPIO48 | GPIO, pull-down | Loop task |
| Button DOWN | 3V3 → | GPIO47 | GPIO, pull-down | Loop task |
| Button LEFT | 3V3 → | GPIO21 | GPIO, pull-down | Loop task |
| Button RIGHT | 3V3 → | GPIO45 | GPIO, pull-down | Loop task |
| Button OK | 3V3 → | GPIO20 | GPIO, pull-down | Loop task |
| Button CANCEL | 3V3 → | GPIO0 | GPIO, pull-down | Loop task |

- Each button connects **3V3** to its GPIO. Never connect a button to 5 V.
- Attach the LoRa antenna before powering the board.
- Use the **UART** connector, not the native USB one: GPIO20 (OK) is a USB
  data pin.
- **GPIO45 (RIGHT)** is a strapping pin. Holding RIGHT at reset stops the
  board from booting.
- **GPIO0 (CANCEL)** is the BOOT pin. Don't press BOOT while holding
  CANCEL: that shorts 3V3 to GND. If the boot log shows `CANCEL ... (held
  at startup, ignored)` with nothing pressed, wire CANCEL from GPIO0 to
  GND and set `cancel_active_low = true`.
- Holding OK while connected is push-to-talk. A separate push-to-talk
  button can be added (`ptt_pin`).

## Firmware design

### Tasks

| Task | Core | Priority | Purpose | Input | Output |
| --- | --- | --- | --- | --- | --- |
| Mic | 1 | 6 | Read the ADC, average to 16 kHz, filter and gate, form 20 ms frames | ADC1 channel 0 | Mic frame queue (4) |
| Speaker | 1 | 6 | Filter, set the volume and limit decoded audio, and feed the sample interrupt | Speaker frame queue (3) | 64 ms sample ring |
| Codec | 0 | 2 | Every Opus encode and decode | Mic frames; received packets when their playout time comes | Encoded packet queue (16); speaker frames |
| LoRa | 0 | 1 | The only user of the Core1121: start it, send, receive, retry after a failure | LoRa send queue (8), DIO9 interrupt | LoRa receive queue (8) |
| Loop | 1 | 1 | Everything else: sessions, connections, relaying, broadcasts, buttons, LCD, serial | ESP-NOW receive queue (64), encoded packets, LoRa receive queue, buttons, serial | ESP-NOW frames, LoRa send queue, LCD |

Two things run outside the tasks and do as little as possible:

- A hardware timer interrupt at 16 kHz takes one sample from the ring and
  writes it as the PWM duty on GPIO8.
- The ESP-NOW receive callback drops frames from MACs that aren't listed
  and copies the rest into the receive queue.

The mic and speaker tasks have the highest priority because a late audio
sample is heard. The codec runs below Wi-Fi on the other core, so encoding
never delays the radio. Each device has one owner: only the loop task
draws on the LCD, and only the LoRa task uses the LoRa SPI bus.

### Data flow

```text
Talking    MAX9814 -> [mic] -> frames -> [codec: Opus encode] -> packets
           -> [loop: add the spare frame, seal, envelope] -> ESP-NOW

Relaying   ESP-NOW -> receive queue -> [loop: check the hop tag, drop
           copies, wait 0-20 ms] -> ESP-NOW

Listening  ESP-NOW -> receive queue -> [loop: open, check for replay, ask
           again for gaps] -> 300 ms playout buffer -> [codec: Opus decode]
           -> [speaker] -> ring -> timer interrupt -> PWM on GPIO8

LoRa text  buttons -> [loop: seal] -> send queue -> [lora] -> Core1121
           Core1121 -> DIO9 -> [lora] -> receive queue -> [loop: open] -> LCD
```

### Handset states

| State | Screen | Leaves when |
| --- | --- | --- |
| Starting | | Self-test, radios and keys are ready → Idle. A failure → Stopped |
| Stopped | Red `Stopped (ID N)` with the reason | Reset only |
| Idle | Menu, lists, Status, Tools, the Broadcast pages | The user picks a board → Asking. A request arrives → Asked |
| Asking | `Connecting` | The other side accepts → Connected. It declines, is busy or goes offline, 30 s pass, or CANCEL → Idle |
| Asked | `Request`, with a beep | OK → Connected. CANCEL, or the asker gives up → Idle |
| Connected | Connected screen | CANCEL on either side, or 6 s of silence (20 s over LoRa) → Idle |

A request from another handset replaces whatever screen is shown. After a
connection ends, the screen says why for 3 s.

While connected over ESP-NOW the voice path has three states:

| State | Enters when | Leaves when |
| --- | --- | --- |
| Listening | The connection starts, or talking or playing ends | OK is pressed, or voice arrives |
| Talking | OK is pressed and nothing is playing | OK is released, or 60 s pass |
| Playing | The other side's voice arrives | Its last frame has played |

If both sides press OK at the same moment, one gives way and plays the
other.

### A protected message

Every message, on either radio, is one frame:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | Version |
| 1 | 1 | Type |
| 2 | 1 | Source board ID |
| 3 | 1 | Destination board ID (255: every board) |
| 4 | 4 | Counter, never reused under one key |
| 8 | n | AES-128-GCM ciphertext |
| 8 + n | 16 | End-to-end tag, which also covers the header |
| 24 + n | 16 | Hop tag: HMAC-SHA256 under the network key |

### Sessions

Each pair of boards shares a compiled-in pair key. No message is encrypted
under it. After every boot the two boards run a three-step handshake:
each sends a fresh 16-byte random nonce, and the session key is
HKDF-SHA256 of the pair key and both nonces. The handshake carries HMAC
tags under a key derived from the pair key, so only the other holder can
complete it.

Two boards hold one session for each way they reach each other: direct
ESP-NOW, the relay and LoRa. Each is keyed separately.

### Relaying

1. Board 1 seals a frame for board 2, puts it in an envelope and sends it
   as an ESP-NOW broadcast. The envelope holds a hop limit of 4
   transmissions and the path so far (`1`).
2. Every board that hears it checks the hop tag, and drops the frame if it
   has seen it in the last 5 s or is already on its path. Otherwise it
   waits a random 0–20 ms and broadcasts it again, with itself added to the
   path and one hop fewer.
3. Board 2 takes the first copy, opens it, and drops the later ones.

Every path is used at once, so there are no routes to set up or repair.
Sessions, presence pings, connection messages, voice and hello all travel
this way.

- Copies are recognised by the hop tag, which can't be made up without the
  network key.
- The random wait keeps two relays that can't hear each other from
  transmitting on top of each other every time.
- A board in a connection doesn't relay: its radio time is for its own
  call. A board that is only asking, or being asked, still relays.

### Voice

Opus makes one 60-byte packet every 20 ms. Three things recover a lost
packet:

1. **A spare.** Each packet also carries the frame before it.
2. **Asking again.** The listener holds 300 ms of audio before playing and
   asks the talker for any frame still missing.
3. **An end message** with a frame count and a CRC-32, so the listener
   knows whether it played everything.

### Broadcasts

Each board makes its own broadcast key at boot and hands it to every other
board inside their session. A broadcast is sealed once under the sender's
key and opens on every board holding it.

A board that hears broadcast voice shows its Broadcast page and follows
the talking board, its host. When the host leaves its page it sends a
close frame 4 times, 60 ms apart, and its listeners go back to the page
they were on.

### LoRa

LoRa has its own sessions, made over LoRa, so it works where ESP-NOW
doesn't reach. A LoRa frame is on the air for about 80–130 ms and the
radio can't hear while it sends, so everything is slower: requests go
every 2 s and heartbeats every 5 s. Retries are spread by a random delay
so two boards don't keep sending at the same moment.

## Security

| Protection | How |
| --- | --- |
| Only the two boards of a connection read it | AES-128-GCM under their session key. No third board, relays included, holds it |
| A new key every boot | The session key comes from the pair key and two fresh random nonces |
| Changed or forged messages are refused | A 16-byte tag covers the message and its header |
| Replays are refused | A counter that is never reused, and a 64-counter window on the receiver |
| Only boards of the network are relayed | A relay checks the hop tag before passing a frame on |
| Only the paired board can start a session | The handshake is tagged under a key derived from the pair key |
| Broadcasts | Sealed under a key the sender makes each boot and gives to each board inside their session |
| Start-up check | The firmware tests AES-GCM, HMAC-SHA256 and HKDF against known answers at boot and stops if one fails |

What it does not protect is listed under [Limitations](#limitations).

## Setting up

### 1. Install the tools

Install [PlatformIO](https://platformio.org/install), as the VS Code
extension or the command line. The tests also need a C++ compiler,
Python 3 and OpenSSL 3.

### 2. Get the code

```sh
git clone <this repository>
cd INF2004-SE19/main
```

Open the `main/` folder itself in PlatformIO, not the repository root.

### 3. Know which board is which

<a id="boards"></a>
Each board is built for its own ID, and checks at boot that its MAC is the
one listed for that ID in `include/network_config.h`.

| Board | Environment | Wi-Fi station MAC |
| --- | --- | --- |
| 1 | `board1` | `E0:72:A1:D8:E2:10` |
| 2 | `board2` | `E0:72:A1:D7:FA:B8` |
| 3 | `board3` | `E0:72:A1:D8:F6:20` |
| 4 | `board4` | `E0:72:A1:D8:0E:48` |
| 5 | `board5` | `E0:72:A1:D5:64:0C` |
| 6 | `board6` | `E0:72:A1:D9:A4:94` |

For any other board, do
[Use a different board](#use-a-different-board) first.

### 4. Build and flash

```sh
pio run                      # builds all six
pio run -e board1 -t upload  # one board per environment: board1 ... board6
pio device monitor           # 921600 baud
```

Before each upload hold **BOOT**, tap **RESET**, release **BOOT**. Tap
**RESET** again afterwards to start the firmware. With several boards
plugged in, find the ports with `pio device list` and add
`--upload-port PORT` to the upload and `--port PORT` to the monitor.

Flash every board from the same build.

### 5. Check the boot log

A board that started shows the menu, and its serial log has these lines:

```text
Crypto self-test passed (AES-GCM, HMAC-SHA256, HKDF, X25519)
Wi-Fi station MAC: E0:72:A1:D8:E2:10
Device 1 (handset), channel 6, 1 Mbps
Pair keys compiled in: 5 in use, 0 for boards not listed or without a MAC
Ready; connect from the menu or with flood connect N.
```

Stay quiet for the first second after boot: the microphone measures the
room's background noise then.

## Using the handset

### Menu

| Row | What it opens |
| --- | --- |
| **ESP-NOW flood** | The list of boards to call |
| **LoRa** | The list of boards to connect to over LoRa |
| **Broadcast** | Talk and "Hello all" to every board |
| **LoRa broadcast** | "Hello all" and "Bye all" over LoRa |
| **Status** | This board's ID and MAC, who is online, the LoRa radio, counters |
| **Tools** | Test settings |

### Buttons

| Screen | UP / DOWN | RIGHT | OK | CANCEL |
| --- | --- | --- | --- | --- |
| Menu and lists | Choose a row | | Select | Back |
| Request | | | Accept | Decline |
| Connecting | | | | Stop asking |
| Connected, ESP-NOW | Volume | Send hello | **Hold to talk** | Disconnect |
| Connected, LoRa | Send Hello / send Bye | | | Disconnect |
| Broadcast | Volume | Send "Hello all" | **Hold to talk to all** | Back |
| LoRa broadcast | Send "Hello all" / "Bye all" | | | Back |
| Status | Scroll | | | Back |
| Tools | Choose a row | | Switch the setting | Back |

In a list, LEFT moves up like UP and RIGHT moves down like DOWN. While
connected, LEFT lowers the volume like DOWN.

### A call

1. On board 1 open **ESP-NOW flood**. Each row is another board; a green
   dot means it is online. Press OK on `Connect Board 2`.
2. Board 2 beeps and shows who is asking. **OK** accepts, **CANCEL**
   declines.
3. Once connected, **hold OK to talk** and release to listen. A short
   beep marks the end of what the other side said.
4. **RIGHT** sends "Hello from Board N", at most once a second.
   **UP/DOWN** set the volume.
5. **CANCEL** disconnects both sides.

The connected screen shows the path the frames take, for example
`Route: 1 > 2, direct` or `Route: 1 > 3 > 2, via Board 3`, and how long
ago the other side was last heard.

**Showing a relay on one desk.** Boards on one desk all hear each other,
so nothing needs relaying. Power a third board, then on board 1 choose
Tools → `Block Board 2` and on board 2 `Block Board 1`. Each now ignores
frames heard directly from the other. Connect again and the route reads
`1 > 3 > 2, via Board 3`.

### LoRa texts

Open **LoRa**. A green dot beside a board means it has answered over
LoRa. Pick a board; it accepts as for a call. While connected, **UP**
sends "Hello from Board N" and **DOWN** "Bye from Board N". Each text
shows `Delivered` once the other board's receipt comes back, or
`Not delivered` after three tries. The screen also shows the connected
board's signal strength (RSSI and SNR).

### Broadcast

**Broadcast** needs no connection and nothing to accept. Hold **OK** to
talk to every board in direct range; **RIGHT** sends "Hello all". The
page shows which boards can open this board's broadcasts
(`Heard by: 2 ok 3 -`).

A board that hears broadcast voice jumps to its own Broadcast page, which
names the talking board as `Host`. It goes back to the page it was on when
the host leaves:

| The host | The listener's banner |
| --- | --- |
| presses CANCEL, or accepts a connection | `Board N ended broadcast` |
| restarts | `Board N restarted` |
| is switched off or goes out of range | `Board N is out of range` (after 6.5 s) |

A listener that presses CANCEL leaves alone. A board in a connection takes
no broadcast, so a call is never talked over.

**LoRa broadcast** sends "Hello all" (UP) and "Bye all" (DOWN) to every
board in LoRa range. `Sent to all` means sent, not heard: these texts have
no receipts.

### Tools

OK switches a row; a red dot means it is on. All of them return to their
defaults at the next reset.

| Row | Effect |
| --- | --- |
| `Block Board N` | Ignore relayed frames heard directly from board N, as if it were out of range |
| `Rate: 1 Mbps` | Switch ESP-NOW between 1 and 6 Mbps. Set every board the same |
| `Mute flood` | Send and receive nothing over ESP-NOW relaying, to show a link loss |
| `Mute LoRa` | The same for LoRa |

### Why a connection ended

| Screen says | Meaning |
| --- | --- |
| `You disconnected` / `They disconnected` | CANCEL on this board, or on the other |
| `Declined` / `You declined` | The request was refused |
| `No answer` | Nobody accepted within 30 s |
| `Stopped asking` / `Missed request` | The asking side pressed CANCEL first |
| `Busy` | The other board is in another connection or request |
| `Connection lost` | Nothing from the other board for 6 s (20 s over LoRa) |
| `Not reachable` | The other board went offline or restarted |

## Serial commands

Type a command at 921600 baud and press Enter. `help` lists them all.

| Group | Commands |
| --- | --- |
| Calls | `flood connect N`, `accept`, `decline`, `disconnect`, `hello`, `link` |
| Voice | `talk [S]`, `test [S]` (a test signal), `stop`, `buffer MS` (60–1000), `red on\|off` (the spare frame) |
| Relay | `flood` (counters, and a `TIMING` line: how long relayed frames wait in this board and in its radio), `flood mute on\|off`, `flood pair on\|off`, `relay block N`, `relay unblock N` |
| LoRa | `lora` (status), `lora connect N`, `lora hello`, `lora bye`, `lora log on\|off`, `lora mute on\|off\|rx` |
| Broadcast | `bcast` (status), `bcast talk [S]`, `bcast test [S]`, `bcast close`, `bcast hello`, `lora bcast hello`, `lora bcast bye` |
| Audio | `+`, `-`, `vol N` (1–32), `m` (mute), `gain X`, `gate R`, `]`, `[`, `mic`, `micdump S`, `tone HZ [MS]`, `sweep`, `r` |
| Buttons | `press NAME [MS]`, `hold NAME`, `release [NAME]`, `ui` (NAME: `up down left right ok cancel ptt`) |
| Board | `report`, `keys`, `radio CH MBPS`, `connect N` (a direct ESP-NOW connection, with no relaying), `reboot` |

`flood pair on`, on the board that talks, sends every other voice frame
with the one before it: 25 packets a second in place of 50, so half the
airtime. A lost packet then costs two frames until they are asked for
again.

## Configuring for other boards

Settings are compiled in. There are four places:

| File | Holds |
| --- | --- |
| `platformio.ini` | The board module, flash layout, libraries, one environment per board ID |
| `include/network_config.h` | Wi-Fi channel, ESP-NOW rate, the table of boards (ID, MAC, name) |
| `include/app_config.h` | Pins, audio, timings, LoRa radio settings, relay settings |
| `shared_keys/e2e_keys.h` | The network key and one key per pair of boards |

The board table, the keys, the channel and the LoRa radio settings must be
the same on every board. After changing one of them, rebuild and flash
**every** board. Run `sh tests/run_handset_tests.sh` after any change: it
checks the board table, the key file and the limits the settings must
keep.

### Use a different board

The firmware refuses to start on a board whose MAC is not in the table.

1. Flash any environment on the new board. It stops with `This MAC is not
   in network_config.h`, and shows its MAC on the LCD and as
   `Wi-Fi station MAC` in the serial log.
2. Put that MAC in the slot you want in `include/network_config.h`:

   ```cpp
   {2, {0xE0, 0x72, 0xA1, 0xD7, 0xFA, 0xB8}, "Board 2"},
   ```

3. Rebuild, flash that slot's environment on the new board, and flash the
   other boards so they list the new MAC.

The keys need no step: a pair key belongs to a pair of IDs, not to a MAC.
The name in the row can be changed too; it must fit 12 characters.

### Use fewer than six boards

Nothing to change. Boards that are absent show as offline. To take a slot
off the menus, set its MAC to all zeros.

### Add a seventh or eighth board

1. Add the pair keys. Existing keys are kept:

   ```sh
   python3 shared_keys/generate_keys.py --boards 7
   ```

2. Add a row to `devices[]` in `include/network_config.h`. IDs must run
   1, 2, 3 ... in order.
3. Add an environment to `platformio.ini`, and its name to `default_envs`:

   ```ini
   [env:board7]
   build_flags = ${env.build_flags} -DE2E_DEVICE_ID=7
   ```

4. Add `board7` to the list in `tests/run_build_tests.sh`, and change
   `--boards 6` to `--boards 7` in `tests/run_handset_tests.sh`.
5. Rebuild and flash every board.

The key tool stops at 8 boards. A seven-board configuration builds; it has
not been run on seven boards.

### Use another ESP32-S3 module

Only the N16R8 has been used. For another ESP32-S3 module, change these
lines of `platformio.ini`:

| Setting | Now | Change when |
| --- | --- | --- |
| `board` | `esp32-s3-devkitc-1` | The board has its own PlatformIO definition |
| `board_upload.flash_size`, `board_build.flash_size` | `16MB` | The flash size differs; then also resize `partitions.csv` |
| `board_build.arduino.memory_type` | `qio_opi` | The PSRAM is not Octal-SPI (`qio_qspi` for Quad-SPI PSRAM) |
| `-DBOARD_HAS_PSRAM` | set | The module has no PSRAM: remove it. In this firmware only `micdump` asks for PSRAM |
| `board_upload.before_reset` | `no_reset` | The board enters download mode by itself: remove the line and skip the BOOT and RESET step |

Then check the pins in `include/app_config.h` against the new board:

- The microphone must be on an ADC1 pin (GPIO1–GPIO10). Change `mic_pin`
  and `mic_adc_channel` together; the channel is the GPIO number minus 1.
- GPIO26–GPIO32 belong to the flash, and GPIO33–GPIO37 to Octal-SPI PSRAM.
- GPIO0, GPIO3, GPIO45 and GPIO46 are strapping pins; GPIO19 and GPIO20
  are the native USB port.

### Change pins or parts

All in `include/app_config.h`:

| To do this | Change |
| --- | --- |
| Move a button, the LCD, the mic, the speaker or the LoRa module | Its `*_pin` constants |
| Add a separate push-to-talk button | `ptt_pin` (wired like the others, 3V3 to the GPIO) |
| Wire CANCEL to GND instead of 3V3 | `cancel_active_low = true` |
| Use an LCD with a CS pin, another rotation, or normal colours | `lcd_cs_pin`, `lcd_rotation`, `lcd_inverted` |
| Start louder or quieter | `speaker_default_volume` (1–32), `mic_default_gain` |
| Make the noise gate stricter | `mic_default_gate_ratio` (1.5–10) |
| Trade delay against lost frames | `playout_default_ms` (60–1000); `relay_playout_ms` for a call that goes through a relay |

### Change the radio settings

| To do this | Change | Notes |
| --- | --- | --- |
| Move off a busy Wi-Fi channel | `channel` in `network_config.h` (1–13) | Every board must match |
| Change the ESP-NOW rate | `rate_mbps` in `network_config.h` | Relayed frames are broadcasts, which get no acknowledgement or retry; 1 Mbps loses the fewest |
| Change the LoRa band, range or power | `lora_mhz`, `lora_spreading_factor`, `lora_bandwidth_khz`, `lora_power_dbm` in `app_config.h` | Every board must match. Use a frequency and power your country allows, and an antenna for that band |
| Let frames travel further, or less far | `relay_max_hops` (2–6) | More hops means more airtime |
| Relay with no wait | `relay_jitter_ms = 0` | Relays that can't hear each other then collide |

### Make new keys

The keys in `shared_keys/e2e_keys.h` are demo keys, committed so that every
board builds with no pairing step. Before any real use:

```sh
python3 shared_keys/generate_keys.py --force --boards 6
```

Then keep the file out of git, and rebuild and flash every board.

**Always pass `--boards`.** The tool's default is 5 and it rewrites the
file for the number it is given, so a run with no `--boards` deletes
board 6's keys.

## Tests

These need no board, and nothing is uploaded.

```sh
sh tests/run_all_tests.sh        # all of them, about 2 minutes
sh tests/run_build_tests.sh      # T1-T4; add board1 ... to build fewer
sh tests/run_host_tests.sh       # T7, T9, T10
sh tests/run_handset_tests.sh    # T5, T6, T8
```

| ID | Test | Passes when |
| --- | --- | --- |
| T1 | Every board's firmware compiles | `pio run -e boardN` succeeds for `board1`–`board6` |
| T2 | No compiler warnings | No `warning` line names a file in `src/` or `include/` |
| T3 | Each board gets its own firmware | No two `firmware.bin` files are the same |
| T4 | The scripts are valid | `tools/*.py` compile, `tests/*.sh` parse |
| T5 | The menu (`tests/cutdown_test.py`) | The six menu rows and the Tools rows are the ones in this file |
| T6 | The settings (`tests/config_test.cpp`) | Board IDs in order, no MAC twice, names fit the screen, the largest voice frame fits a relay envelope, the timings fit each other |
| T7 | The relay envelope and duplicate filter (`tests/relay_frame_test.cpp`) | A copy is dropped; a replayed frame doesn't block a new one; a raised hop count is refused; a frame is sent 4 times and no more |
| T8 | Crypto, sessions, connections, LoRa texts, voice link, audio filters, key file | Each suite prints `All ... passed` |
| T9 | A hello replayed during a handshake (`tests/session_test.cpp`) | Up to three replayed hellos don't stop the session; four do, and a new handshake recovers |
| T10 | Sessions are keyed per radio path (`tests/session_test.cpp`) | A handshake made for LoRa is refused over ESP-NOW, and the other way round |

Scripts in `tools/` drive flashed boards over serial and save their logs.
The docstring of each has the command.

| Script | What it does |
| --- | --- |
| `tools/relay_test.py` | Calls between boards 1 and 2: connect, talk, hello, disconnect; `middle` sends everything through board 3 |
| `tools/broadcast_test.py` | Broadcast voice and texts between boards 1 and 2 |
| `tools/check_streams.py` | Compares a talker's and a listener's log: every frame played, CRC equal |
| `tools/mic_dump.py` | Records the microphone to a WAV file |
| `tools/capture_logs.py` | Captures several serial ports with shared timestamps |

## Limitations

**Calls**

- **Half duplex.** One side talks at a time, up to 60 s per press.
- **One connection per board**, over either radio.
- **A board in a connection doesn't relay.** Boards that reach each other
  only through it show as offline until it disconnects, and a call relayed
  through it ends as `Connection lost`.
- **At most 4 transmissions per frame**, so up to three relays on the way.
- **Airtime grows with every board in range.** Each board that hears a
  relayed frame sends it once more: one talker's 50 packets a second
  become about 250 with six boards in range, roughly 40% of the channel
  at 1 Mbps (an estimate). `flood pair on` halves it.
- **One fixed channel, shared with Wi-Fi.**
- **Voice is delayed.** It plays 300 ms after it arrives, or 800 ms when
  it came through a relay, plus up to 20 ms at each relay.
- **Not measured.** No loss, delay or range figures have been recorded
  for this firmware.

**Broadcast and LoRa**

- **Broadcasts are not relayed.** They reach only boards in direct range,
  and a board in a connection ignores them.
- **A listener can miss the close.** If all four close frames are lost it
  stays on the Broadcast page until CANCEL.
- **LoRa carries text only**, and only the fixed "Hello" and "Bye". There
  is no keyboard and no free text.
- **LoRa is direct only**, at 0 dBm.

**Configuration**

- **Everything is compiled in**: board IDs, MACs, keys, channel. Adding or
  replacing a board means rebuilding and reflashing every board. There is
  no pairing step.
- **Every board must run the same build and the same key file.** Boards
  that differ refuse each other's handshake (`bad handshake tag`).
- **Nothing is saved.** Volume, mute, rate, playout buffer and the Tools
  settings return to their defaults at every reset.
- **Six boards**, eight at most with the key tool.
- **Only the ESP32-S3 N16R8 has been used.**

**Hardware**

- **The UART connector only.** OK is on a USB data pin.
- **RIGHT held at reset stops the boot**, and CANCEL shares the BOOT pin.
- **No power saving.** Wi-Fi sleep is off and nothing is put to sleep.
- **The microphone needs a quiet first second** after boot.

**Security**

- **The keys are demo keys in a public repository.** Anyone with it can
  read the traffic until new keys are made.
- **Who talks to whom is visible.** The header and the frame length are
  in the clear; the content is not.
- **No protection against jamming.**
- **An old frame can be relayed again.** A recorded frame sent later is
  passed on once more by idle boards. The destination refuses it as a
  replay; the cost is airtime.
- **The relay envelope is not signed.** Its hop count and path can be
  rewritten, which can shorten how far a frame travels or change the
  route shown. It can't make a frame travel further.
- **No forward secrecy.** Whoever holds a pair key can work out every
  session key made from it.
- **Keys sit in plain flash.** A lost board gives up the network key and
  its own pair keys.
- **A broadcast key is shared.** Any board holding it could send a
  broadcast, or a close, in the sender's name.

## Troubleshooting

| What you see | Cause and fix |
| --- | --- |
| Upload can't connect | Hold BOOT, tap RESET, release BOOT, then upload again |
| Nothing on the serial monitor | Use the UART connector at 921600 baud, and tap RESET after an upload |
| The board doesn't start at all | RIGHT was held at reset (GPIO45), or BOOT is still held |
| `Stopped`: `Wrong boardN environment for this board` | The log line above it says which board this is. Flash that environment |
| `Stopped`: `This MAC is not in network_config.h` | [Use a different board](#use-a-different-board) |
| `Stopped`: `Crypto self-test failed` | Rebuild from a clean checkout |
| A board never shows as online | It is off, on another channel, or reachable only through a board that is in a connection |
| A board stays offline and the log says `bad handshake tag` | The two boards have different builds or keys. Flash both from the same build |
| The LoRa row has a red dot | Check the Core1121 wiring and antenna. The radio is tried again every 5 s and the rest of the handset keeps working |
| No sound | Raise the volume with UP on the connected screen; `m` on serial toggles mute |
| `Can't talk` and a low beep | Push-to-talk was refused, for example with no session to the other board yet. Release OK and press again |
| Voice breaks up with many boards on | Too much relayed traffic: try `flood pair on` on the talking board, or switch off boards that aren't needed |
| CANCEL acts by itself at boot | See the GPIO0 note under [Wiring](#wiring) |

## Source layout

| Path | What |
| --- | --- |
| `src/main.cpp` | Start-up, the receive loop, serial commands. The `.inc` files below are parts of it |
| `src/handset.inc` | Connections, push-to-talk voice, presence, audio commands |
| `src/relay.inc`, `src/relay_frame.cpp` | Relaying; its envelope and duplicate filter |
| `src/lora.inc`, `src/lora_radio.cpp`, `src/lora_text.cpp` | LoRa sessions and connections; the LoRa task; texts and receipts |
| `src/broadcast.inc` | Broadcasts and their keys |
| `src/ui.inc` | Screens and buttons |
| `src/secure_message.cpp`, `src/session.cpp`, `src/kdf.cpp`, `src/control_frame.cpp`, `src/crypto_backend_mbedtls.cpp` | Sealing, the handshake, key derivation |
| `src/call.cpp` | The connection state machine |
| `src/voice_link.cpp`, `src/opus_voice.cpp`, `src/codec.cpp` | Spare frames and asking again; Opus |
| `src/mic.cpp`, `src/speaker.cpp`, `src/audio_dsp.cpp` | Microphone, speaker, filters |
| `src/radio.cpp` | ESP-NOW |
| `include/app_config.h`, `include/network_config.h` | The settings |
| `shared_keys/` | The key file and its generator |
| `tests/`, `tools/` | Tests that need no board; PC scripts that drive flashed boards |

The team's own code is under the [MIT licence](../LICENSE).
