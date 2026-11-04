# LCD + LoRa together: ST7789 and Core1121 on separate SPI buses

Step 2 of the LoRa redo. It runs the LCD and the LoRa radio on one firmware,
with no menu. The earlier LoRa handset build froze on board 1 with
`LR1121 init failed, RadioLib code: -705` (the radio didn't answer SPI
commands in time) and never drew its menu. Board 1's radio works on its own in
[lora_encrypt_test](../lora_encrypt_test/), so this test checks whether the
radio still works with the LCD running beside it.

Each board sends the other the pre-built texts **"Hello from Board N"** and
**"Bye from Board N"** in turn, one every 2–3 s (random spacing, so the two
rarely transmit at once), and shows on the LCD:

- **LoRa: OK** (green), or **LoRa: ERR code** (red) with what failed, or
  **BUSY stuck high** if the radio never came out of reset
- Texts sent, heard and missed (gaps in the other board's numbering)
- RSSI and SNR of the last text, and how long ago it arrived
- **Last received:** the text itself, e.g. `Hello from Board 2`

A radio error never stops the board. The screen shows it and the radio is
retried every 5 s. On air a text carries its send number after it
(`Hello from Board 1 #7`) for the missed count; the screen shows only the
text. Texts are plaintext, since this test is about the buses and not the
encryption.

## Radio settings

915 MHz, 125 kHz, SF7, CR 4/5, sync word 0x12, 8-symbol preamble, CRC on,
0 dBm on the HP path: the same as `lora_encrypt_test`, which works on both
boards. Settings are in `include/app_config.h`; both boards must match.

## Buses and wiring

| Part | Bus | Pins |
| --- | --- | --- |
| Core1121 LoRa | `SPI` (SPI2), as in `lora_encrypt_test` | SCK 7, MISO 5, MOSI 6, CS 15, RESET 16, BUSY 17, DIO9 18 |
| ST7789 LCD | `HSPI` (SPI3) | SCL 12, SDA 11, DC 9, RES 10, BLK 14 (no CS) |
| Speaker | — | GPIO8 held low (quiet) |

The earlier failed build had the buses the other way round (LoRa on HSPI).
Set `lora_on_hspi = true` in `include/app_config.h` to try that layout.
Buttons, mic and speaker can stay wired; this firmware doesn't use them.

## Flash and run

You flash. From this folder (hold BOOT, tap RESET, release BOOT before each
upload; tap RESET after):

```sh
~/.platformio/penv/bin/pio run -e board1 -t upload --upload-port /dev/cu.usbmodem5B5F0216001
~/.platformio/penv/bin/pio run -e board2 -t upload --upload-port /dev/cu.usbmodem5B5F0215971
```

Serial log (115200 baud), one terminal per board:

```sh
~/.platformio/penv/bin/pio device monitor --port /dev/cu.usbmodem5B5F0216001
~/.platformio/penv/bin/pio device monitor --port /dev/cu.usbmodem5B5F0215971
```

Each boot prints the bus layout, `LCD OK`, how long BUSY took to go low after
reset, then `LORA OK ...` or `LORA ERROR <step>: <code>`. Every text sent and
received is logged with RSSI and SNR.

## Pass and what a failure means

**Pass:** both screens show `LoRa: OK`, and over 2 minutes each board hears
roughly as many texts as the other sent, and `Last received` alternates
between `Hello from Board N` and `Bye from Board N`, with only a few missed (an occasional
collision is normal).

| What you see | Likely cause |
| --- | --- |
| Both OK, texts both ways | The LCD and LoRa coexist; the old freeze was in the integration's startup or menu code |
| Board 1 `ERR -705`, BUSY went low | Radio out of reset but not answering on SPI: check MISO/MOSI/SCK/CS, then try `lora_on_hspi = true` |
| Board 1 `BUSY stuck high` | Radio not coming out of reset: check RESET (16), BUSY (17), 3V3 and GND to the Core1121 |
| `ERR` only after the LCD starts, fine in `lora_encrypt_test` | A conflict with the LCD: bus, pins or power (try with the backlight wire off) |
| OK on both, nothing heard | Frequency or settings mismatch, antenna, or one board on an old build |

Status: **both environments build. Not yet run on hardware.**
