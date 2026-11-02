# Six-button menu on the ST7789 LCD

Standalone PlatformIO / Arduino project for the ESP32-S3 N16R8 used by the
other tests. Builds on [dpad_lcd_test](../dpad_lcd_test/) with the same
buttons and LCD wiring, but uses them to navigate a menu on the 240 x 240
1.3 inch ST7789 LCD: a main menu with four items, each opening its own page.

## Wiring

Identical to [dpad_lcd_test](../dpad_lcd_test/README.md#wiring). With the
board unplugged, wire each normally-open tactile button between 3V3 and
its GPIO. The firmware enables the internal pull-downs, so released is LOW
and pressed is HIGH. Never connect a button to 5 V.

| Button | GPIO |
| --- | --- |
| Up | 48 |
| Down | 47 |
| Left | 21 |
| Right | 45 |
| Confirm | 20 |
| Cancel | 0 |

| LCD pin | ESP32-S3 connection |
| --- | --- |
| VCC | 5 V |
| SCL / SCK | GPIO12 |
| SDA / MOSI | GPIO11 |
| RES / RST | GPIO10 |
| DC | GPIO9 |
| BLK | GPIO14 |
| GND | GND |

The pin side effects described in `dpad_lcd_test` apply here too:

- GPIO20 (confirm) is native USB D+. Use the USB-to-UART connector.
- Holding right (GPIO45) during reset stops the board from booting.
- Pressing BOOT while holding cancel (GPIO0) shorts 3V3 to GND.

If cancel is stuck pressed because of a board pull-up on GPIO0, the serial
pin list marks it `(held at startup, ignored)`. Rewire cancel between GPIO0
and GND and set `cancel_active_low = true` in `include/app_config.h`.

## Build and run

```sh
cd dpad_menu_test
pio run
pio run -t upload
pio device monitor
```

The project uses the USB-to-UART connector at 115200 baud. For manual
download mode, release all six buttons, hold BOOT, press and release
RESET, then release BOOT before upload. Press RESET after upload to start
the application.

## Expected behavior

The main menu lists Hello World, Counter, System Info and About, with the
selected row filled orange.

| Screen | Button | Action |
| --- | --- | --- |
| Main menu | Up / Left | Previous item, wrapping to the last |
| Main menu | Down / Right | Next item, wrapping to the first |
| Main menu | Confirm | Open the selected item |
| Counter | Up / Right / Confirm | Add 1 |
| Counter | Down / Left | Subtract 1 |
| Any page | Cancel | Return to the main menu |

System Info shows the chip model and CPU frequency, and refreshes free
heap and uptime once per second. The counter keeps its value after leaving
the page, until the next reset.

Each press fires once after its input stays stable for 30 ms; holding a
button does not repeat it. A button already held at startup is ignored
until it is released and pressed again. Serial output logs every press
and the resulting action:

```text
PRESSED  DOWN
Menu: Counter
PRESSED  CONFIRM
Open: Counter
PRESSED  UP
Counter: 1
PRESSED  CANCEL
Back to menu
```

If the serial log shows a press but the screen does not change, check the
LCD wiring. If a button never appears in the log, see the troubleshooting
notes in `dpad_lcd_test`. The LCD runs at 10 MHz like the other LCD tests;
raise `lcd_spi_hz` for faster redraws once the wiring is known to be good.

Hardware validation is pending.
