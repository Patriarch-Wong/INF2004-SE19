# Six-button D-pad test on the ST7789 LCD

Standalone PlatformIO / Arduino project for the ESP32-S3 N16R8 used by the
other tests. Extends [button_test](../button_test/) to six debounced
buttons: a four-way D-pad plus confirm and cancel. The 240 x 240 1.3 inch
ST7789 LCD shows which buttons are held and names the most recent press.

## Wiring

With the board unplugged, wire each normally-open tactile button between
3V3 and its GPIO. No external resistors are needed: the firmware enables
the internal pull-downs, so released is LOW and pressed is HIGH. Never
connect a button to 5 V. As in `button_test`, use one leg from each
internally connected pair (opposite diagonal corners on a standard
four-leg switch).

| Button | GPIO |
| --- | --- |
| Up | 48 |
| Down | 47 |
| Left | 21 |
| Right | 45 |
| Confirm | 20 |
| Cancel | 0 |

```text
3V3 ---- normally-open button ---- GPIOx     (one per button)
```

These pins avoid octal PSRAM (35-37), the onboard SD slot (38-40), and the
pins used by the mic, speaker and LoRa tests, but three of them have side
effects:

- GPIO20 (confirm) is native USB D+, so the native USB connector cannot be
  used for upload or serial. Use the USB-to-UART connector.
- GPIO45 (right) is the VDD_SPI strapping pin. Holding right during reset
  drives it high, selecting 1.8 V flash power, so the board does not boot.
  Release right and reset again.
- GPIO0 (cancel) is the BOOT strapping pin, shared with the onboard BOOT
  button. Holding cancel during reset forces a normal boot; it no longer
  enters download mode. The BOOT button connects GPIO0 to GND, so
  pressing BOOT while holding cancel shorts 3V3 to GND. Do not press both.

If cancel shows as pressed with nothing held, the board's GPIO0 pull-up is
overpowering the internal pull-down. Rewire cancel between GPIO0 and GND
and set `cancel_active_low = true` in `include/app_config.h`. Cancel then
uses the internal pull-up, the onboard BOOT button also registers as
cancel, and the 3V3-to-GND short cannot happen.

On some DevKitC-1 revisions GPIO48 (up) also drives the onboard RGB LED.
GPIO numbers are the `IO` labels, not physical header positions. Change
them in `include/app_config.h`.

The LCD wiring matches the
[wireless_stream](../button_lcd_test/wireless_stream/) player:

| LCD pin | ESP32-S3 connection |
| --- | --- |
| VCC | 5 V |
| SCL / SCK | GPIO12 |
| SDA / MOSI | GPIO11 |
| RES / RST | GPIO10 |
| DC | GPIO9 |
| BLK | GPIO14 |
| GND | GND |

The seven-pin module has no CS (`lcd_cs_pin = -1`) and runs in SPI mode 3
at 10 MHz. The firmware drives BLK high to turn the backlight on. VCC on
5 V relies on the module's onboard 3.3 V regulator; the signal lines stay
at 3.3 V.

## Build and run

```sh
cd dpad_lcd_test
pio run
pio run -t upload
pio device monitor
```

The project uses the USB-to-UART connector at 115200 baud; native USB is
unavailable because confirm uses GPIO20. For manual download mode, release
all six buttons, hold BOOT, press and release RESET, then release BOOT
before upload. Press RESET after upload to start the application.

## Expected behavior

The screen shows a D-pad on the left and round `OK` (confirm) and `X`
(cancel) buttons on the right. A held button fills yellow; it returns to an
outline on release. The large text at the top names the most recent press,
yellow while held and gray after release. Several buttons can be held at
once; each is tracked independently.

Each input change must remain stable for 30 ms. Holding a button does not
repeat it. A button already held during startup counts as a press after
debouncing. Serial output mirrors the screen:

```text
PRESSED  UP
RELEASED UP | held 183 ms
PRESSED  CONFIRM
RELEASED CONFIRM | held 412 ms
```

If a button shows as permanently pressed, its two wires share a contact
pair or are shorted on the breadboard; for cancel, see the GPIO0 note
above. If one never registers, check its GPIO number and that its other
leg reaches 3V3, not GND. If the screen is mirrored, offset or the colors
are inverted, adjust `lcd_rotation` or `lcd_inverted` in
`include/app_config.h`; a dark screen calls for checking BLK and VCC.

Hardware validation is pending.
