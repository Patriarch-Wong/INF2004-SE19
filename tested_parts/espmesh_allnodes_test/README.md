# ESP-MESH(ALLNODES)

This ESP32-S3 N16R8 handset project uses ESP-NOW for every AllNodes hop. Each board can send, receive, and relay. A call uses one confirmed path in both directions, reversing the board order for return traffic. The handset also retains its native ESP-MESH, flood mesh, and LoRa modes. [INF2004-SE19 baseline at `1ba7d6c`](https://github.com/Patriarch-Wong/INF2004-SE19/tree/1ba7d6c8e7020609750a28affd683b250caccf53/tested_parts/handset_espnow_mesh_integration_test)

## Build and flash

Open this folder in PlatformIO. `src/ESP_MESH_ALLNODES.ino` is the Arduino entry point. Keep the whole folder together, including the other sources, headers, and `partitions.csv`. The keys are the repository's shared demo keys in [`../shared_keys/e2e_keys.h`](../shared_keys/README.md), so build it inside `tested_parts`.

Choose `board1` through `board5` to match the physical handset, and `sniffer` for a spare board. Flash this version on all participants so they share the AllNodes packet format and board list.

The board MAC table in `include/network_config.h` is the one in `handset_peer_mesh_integration_test`:

| Board | Environment | Wi-Fi station MAC |
| --- | --- | --- |
| 1 | `board1` | `E0:72:A1:D8:E2:10` |
| 2 | `board2` | `E0:72:A1:D7:FA:B8` |
| 3 | `board3` | `E0:72:A1:D8:F6:20` |
| 4 | `board4` | `E0:72:A1:D8:0E:48` |
| 5 | `board5` | `E0:72:A1:D5:64:0C` |
| spare | `sniffer` | any; the sniffer has no MAC check |

If a handset says its MAC is unknown or the board ID is wrong, read `Wi-Fi station MAC` from its boot log or use `esptool read_mac`, update that board's entry, then rebuild and flash every handset.

Connect to the board's UART USB port. From the PlatformIO terminal, substitute your environment and serial port for the examples below. Before upload, close the monitor, hold **BOOT**, tap **RESET**, then release **BOOT**. Tap **RESET** again after uploading.

```powershell
pio run -e board1
pio run -e board1 -t upload --upload-port COM11
pio device monitor --port COM11 --baud 921600
```

## Check the same path in both directions

Use Board 1 as A, 2 as B, 3 as C, 4 as D, and 5 as E. The example has two paths, `1 ↔ 3 ↔ 2` and `1 ↔ 4 ↔ 5 ↔ 2`. With the boards within radio range, use the serial commands below to simulate that layout. The blocks apply to direct traffic heard from the named board, so set them on both ends of each excluded link.

| On board | Run these commands separately |
| --- | --- |
| 1 | `relay block 2`, `relay block 5` |
| 2 | `relay block 1`, `relay block 4` |
| 3 | `relay block 4`, `relay block 5` |
| 4 | `relay block 2`, `relay block 3` |
| 5 | `relay block 1`, `relay block 3` |

1. To force the first call through Board 3, also run `relay block 4` on Board 1 and `relay block 1` on Board 4. Check `routes` on Board 1 once discovery settles.
2. On Board 1, run `routed connect 2`. On Board 2, run `accept`. You can also select **ESP-MESH(ALLNODES)** on the handset menu and accept with **OK**.
3. Run `hello` on Board 1, then run `hello` on Board 2 as the reply. Match the `ROUTED TEXT path tx`, `hop`, and `rx` log lines by their `id=` within each direction. The observed board order should be `1 → 3 → 2`, then `2 → 3 → 1`.
4. Restore the 1–4 link with `relay unblock 4` on Board 1 and `relay unblock 1` on Board 4, then power off Board 3. Wait for the call to end and use `routes` to check discovery. Start a fresh call and repeat both `hello` commands. Expect `1 → 4 → 5 → 2`, then `2 → 5 → 4 → 1`. Run `disconnect` to end the call.

## Watch the hops with the sniffer

The `sniffer` environment (`src/sniffer`) is a keyless passive sniffer for a spare board. It never transmits and holds no keys, so it shows what an outsider sees. Flash it, keep the board within radio range of every handset, and open its serial monitor. Nothing else is needed on the computer.

```sh
pio run -e sniffer -t upload --upload-port /dev/cu.usbmodem5B420192791
pio device monitor --port /dev/cu.usbmodem5B420192791 --baud 921600
```

It prints one line per frame a board sends, with an encryption verdict read from the clear headers only:

| Line | Meaning |
| --- | --- |
| `ESPNOW … enc=aes-gcm msg=allnodes:1>3>2@1:id=57:ttl=2:text:1>2:#5` | An AllNodes frame on the route 1 > 3 > 2, this copy sent to the board at index 1 (Board 3). Inside it, an encrypted text from 1 to 2 |
| `RELAY … net=allnodes path=1>3>2 id=57 hop=2/2 link=B3>B2 delay_us=1834` | The same frame (same origin and ID) heard again on its second hop, 1.8 ms after the first |
| `ESPNOW … enc=clear-route msg=route-request:1?2:id=4:ttl=3:path=1>3` | Route discovery: board IDs only, signed, clear by design |
| `ESPNOW … msg=flood:1>3:…` | A flood mesh frame and the boards it has passed |

Every 5 seconds, and on `status`, it prints totals:

| Line | Meaning |
| --- | --- |
| `SENT … verdict=ALL-DATA-ENCRYPTED` | Per board, frames encrypted, clear by design, and unencrypted |
| `ROUTES allnodes 1>3>2:120(hops_heard=120/118 complete=118 avg=2.1ms,…)` | Per path: frames, how many were heard at each hop, and the delay from hop 1 to the last hop |
| `TWOWAY 1>3>2+2>3>1:same-boards=yes` | The return path is the same boards reversed |
| `PATHS \| espnow_direct B1>B3:124(acked=118)` | ESP-NOW unicasts per link, and how many were followed by the receiver's Wi-Fi ACK |
| `HEARD B1:532(rssi_avg=-48,min=-61)` | How strongly the sniffer hears each board |

For the two-way check above, run `clear` on the sniffer, send `hello` each way, then `status`: `TWOWAY` should show `1>3>2+2>3>1:same-boards=yes`.

If `hops_heard` is lower on one hop, either the sniffer missed frames from that relay or the relay dropped them. `HEARD` shows a weak board; move the sniffer between the boards and compare. `help` lists the commands: `relay off` hides RELAY lines, `espnow off` hides frame lines and keeps the totals, `boards any` adds frames sent to a board by others.
