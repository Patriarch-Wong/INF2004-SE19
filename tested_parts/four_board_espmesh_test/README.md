# SE19 four-board ESP-MESH demo

```text
Board 1  ←── WPA2 hop ──→  Board 3  ←── WPA2 hop ──→  Board 2
handset                   root and relay             handset

Board 4 listens to both hops and sends a capture to your computer.
```

Boards 1 and 2 can both send messages. Board 3 is the fixed mesh root. Each handset joins it directly, so a mesh Hello from one handset to the other goes through Board 3. The sniffer listens separately and needs no mesh keys. [ESP-WIFI-MESH routing guide](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-guides/esp-wifi-mesh.html#routing-tables)

## 1. Build and upload the four boards

This is one PlatformIO project, converted from the Arduino kit
`SE19_Four_Board_ESP_MESH_Arduino`. Boards 1 to 3 run the same handset
firmware built for their board ID; the sniffer is separate firmware in
`src/sniffer`.

| Board | Environment | Role |
| --- | --- | --- |
| 1 | `board1` | Handset |
| 2 | `board2` | Handset |
| 3 | `board3` | Fixed root and relay |
| 4 | `sniffer` | Passive sniffer |

```sh
pio run                          # build all four
pio run -e board1 -t upload      # then board2, board3, sniffer
pio device monitor               # 921600 baud
```

Add `--upload-port` and `--port` when more than one board is plugged in.
These boards need **BOOT + RESET** by hand before each upload: hold **BOOT**,
tap **RESET**, release **BOOT**, and tap **RESET** again after the upload.

`platformio.ini` pins the same setup as the sibling projects: `espressif32@7.1.3`
(Arduino-ESP32 2.0.17), 16 MB QIO flash, OPI PSRAM, the 16 MB partition table,
USB CDC on boot disabled, Adafruit ST7735 and ST7789 Library 1.11.0, RadioLib
7.7.1 and the pinned libopus. The kit named Arduino-ESP32 3.3.12 and newer
library versions; the sources build on both.

The keys are the repository's shared demo keys in
[`../shared_keys/e2e_keys.h`](../shared_keys/README.md), committed on purpose,
and the board MAC table is `include/network_config.h`. Board 2 uses its original station MAC (`E0:72:A1:D7:FA:B8`), matching the other projects. If a handset says its
MAC is unknown or the board ID is wrong, read `Wi-Fi station MAC` at startup,
update that board's entry in `include/network_config.h`, and upload boards 1
to 3 again. A capture with these keys is suitable for this lab test. Anyone
with the keys can perform the same decryption check.

## 2. Start the sniffer before the mesh joins

Use **921600 baud** and **New Line** in the Serial Monitors for Boards 1, 2, and 3. Close any Serial Monitor on **Board 4** because the capture script needs that port. Keep Board 4 within radio range of all three mesh boards.

Run this from the project folder. Replace the port with Board 4's (`COM7` on Windows). Install `pyserial` once if needed.

```sh
python3 -m pip install pyserial
python3 tools/capture_mesh.py /dev/cu.usbserial-XXXX mesh-relay.pcap --seconds 300
```

When it says `Capturing channel 11`, reset Board 3. Wait for `MESH started as root`, then reset Boards 1 and 2 so the capture includes both WPA2 joins and both handsets' encrypted session setup. Type `mesh` on each board. Boards 1 and 2 should each show `root Board 3` and `parent Board 3`. Board 3 should list both children.

The capture defaults to the repository addresses. For custom addresses, add `--mac` once per address. Include Board 1 STA, Board 2 STA, Board 3 STA, and Board 3 SoftAP. On the project's boards with globally assigned STA addresses, the SoftAP address sets the local bit in the first octet, for example `E0` becomes `E2`.

## 3. Send a Hello in each direction

| Order | Board | Serial command |
| --- | --- | --- |
| 1 | 1 | `mesh connect 2` |
| 2 | 2 | `accept` when the request appears. Wait for both to report `MESH LINK connected`. |
| 3 | 2 | `mesh cryptotest` |
| 4 | 1 | `hello` after Board 2 reports the test is armed |
| 5 | 1 | `mesh cryptotest` |
| 6 | 2 | `hello` after Board 1 reports the test is armed |

Each receiving board should print `MESH CRYPTOTEST PASS`. Press **Ctrl+C** in the capture window to save the PCAP. Use a new capture filename on subsequent runs.

## 4. Verify the relay path and encryption

With **Node.js** and **Wireshark with TShark** installed, run this from the project folder.

```sh
node tools/inspect_relay.mjs mesh-relay.pcap ../shared_keys/e2e_keys.h
```

For custom addresses, add three colon-separated MAC arguments in this order: Board 1 STA, Board 3 SoftAP, Board 2 STA.

The report opens the captured WPA2 traffic with the matching network key. It checks that the same AES-GCM-authenticated Hello appears in two distinct capture frames on the expected physical Wi-Fi hops, in each direction. It also checks that the wrong session key and altered ciphertext are rejected. The expected summary is:

```text
PASS: Authenticated Hello crossed both WPA2 radio hops through Board 3 in both directions.
```

If the result is `INCONCLUSIVE`, place the sniffer where it can hear every board, start a fresh capture before restarting the mesh, and check that both handsets report Board 3 as their parent. The sniffer's drop count helps spot missing packets. [Wireshark guide to capturing WPA2 handshakes](https://www.wireshark.org/docs/wsug_html_chunked/Ch80211Keys.html#Ch80211KeysGotchas)
