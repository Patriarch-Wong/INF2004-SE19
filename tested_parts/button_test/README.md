# Four-pin tactile button test

Standalone PlatformIO / Arduino project for the ESP32-S3 N16R8 used by the
other tests. Tests one normally-open, four-legged tactile pushbutton.
An LED on GPIO5 lights while the debounced button is pressed.

## Wiring

With the board unplugged, connect **GPIO4** to one switch contact and **GND**
to the other. Leave the two unused legs disconnected. No external resistor
is needed: the firmware enables the internal pull-up, so released is HIGH
and pressed is LOW. Do not connect the button to 5 V or 3.3 V.

The four legs form two internally connected pairs. Use one leg from each
pair; two legs in the same pair will read as permanently pressed. On a
standard four-leg tactile switch, opposite diagonal corners work. Confirm
with a continuity meter if the pin arrangement is uncertain: the selected
legs should connect only while pressed. See
[Adafruit's switch wiring guide](https://learn.adafruit.com/make-it-switch/what-about-breadboard-switches).

```text
GPIO4 ---- normally-open button ---- GND
```

GPIO4 is the header marked `4` / `IO4`, not the fourth physical header pin.
See the [Espressif board pinout](https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32s3/hw-reference/esp32s3/user-guide-devkitc-1.html).
Change `button_pin` in `include/app_config.h` if using another free GPIO.

Connect the external LED with a series resistor:

```text
GPIO5 ---- 330 ohm resistor ---- LED anode (+)
GND -------------------------- LED cathode (-)
```

The anode is usually the longer leg; the cathode is usually the shorter leg
beside the flat edge of the LED body. This wiring turns the LED on when
GPIO5 is HIGH. Do not connect a bare LED directly without a resistor.

## Build and run

```sh
cd button_test
pio run
pio run -t upload
pio device monitor
```

The default environment uses the USB-to-UART connector at 115200 baud.
For manual download mode, hold BOOT, press and release RESET, then release
BOOT before upload. Press RESET after upload to start the application.
Use `--upload-port /dev/cu.YOUR_PORT` or `--port /dev/cu.YOUR_PORT` for the
upload or monitor command when multiple boards are connected.

For the native USB connector:

```sh
pio run -e esp32-s3-n16r8-usb -t upload
pio device monitor -e esp32-s3-n16r8-usb
```

## Expected behavior

Each input change must remain stable for 30 ms. A press increments the count
once; holding the button does not repeat it. Release prints the elapsed time
between the debounced press and release. The LED stays lit while held and
turns off on release. The count resets on reboot. A button
already held during startup counts as the first press after debouncing;
its duration starts at detection, not before boot.

Example output (illustrative):

```text
PRESSED  #1
RELEASED #1 | held 247 ms
PRESSED  #2
RELEASED #2 | held 1520 ms
```

Try several short presses and one long hold: each should produce exactly one
press and one release. Pulses shorter than the debounce interval are ignored.
If the input stays pressed, check that the wires use different contact pairs
and that the breadboard does not short them together. If no presses register,
check the GPIO number and ground connection.

UART and native USB builds passed with `pio run -e esp32-s3-n16r8
-e esp32-s3-n16r8-usb`. Hardware validation is pending.
