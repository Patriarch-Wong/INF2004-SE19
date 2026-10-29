# LoRa broadcast send and receive: ESP32-S3 + Waveshare Core1121

Every board runs the same firmware: it broadcasts a numbered plaintext message
periodically and listens for broadcasts from other boards between transmissions.
This is direct LoRa communication: no gateway, Internet connection, LoRaWAN keys,
pairing, or receiver address list is needed.

There is one PlatformIO environment, `esp32-s3-n16r8`, and one behavior. Upload
the same project to every board without selecting sender or receiver roles.

## Build and upload

The ESP32-S3 N16R8 settings are retained: 16 MB QSPI flash, 8 MB OPI PSRAM,
custom `partitions.csv`, and PlatformIO espressif32 7.1.3.

From this project directory, find the serial ports:

```sh
pio device list
```

Build without uploading:

```sh
pio run
```

Use the UART/COM connector and replace `PORT_A` and `PORT_B` with the actual
ports, such as `COM5` and `COM6` on Windows. Upload the same firmware to each:

```sh
pio run -t upload --upload-port PORT_A
pio run -t upload --upload-port PORT_B
```

Open two terminals, one monitor per board:

```sh
pio device monitor --port PORT_A
pio device monitor --port PORT_B
```

Both monitors use 115200 baud. For the board's native USB connector, change
`-DARDUINO_USB_CDC_ON_BOOT=0` to `-DARDUINO_USB_CDC_ON_BOOT=1` in
`platformio.ini`, then rebuild and upload. This changes the serial interface
within the same environment.

Close a port's monitor before uploading to it. If needed, hold BOOT, tap RESET,
release BOOT, then upload. Reset after opening the monitor to see startup
diagnostics.

The monitor keeps DTR and RTS inactive (`monitor_dtr = 0`, `monitor_rts = 0`).
If the output says `DOWNLOAD(USB/UART0)` and `waiting for download`, release
BOOT and tap the ESP32's EN/RST button once with the monitor open. If it stays
in download mode, stop the monitor, unplug/reconnect the board with BOOT
released, reopen the monitor, and tap EN/RST again. Check whether BOOT is
stuck or GPIO0 is connected to ground if this persists. The radio's RESET
wire to GPIO16 is separate from the ESP32's EN/RST button.

### Windows long paths

If RadioLib installation fails copying examples in a deeply nested checkout,
use short temporary paths for dependencies and build output. In PowerShell,
run these before the commands above in the same terminal:

```powershell
$env:PLATFORMIO_LIBDEPS_DIR = Join-Path $env:TEMP 'inf2004-lora-libdeps'
$env:PLATFORMIO_BUILD_DIR = Join-Path $env:TEMP 'inf2004-lora-build'
```

## Broadcast behavior

Change `broadcastText`, `broadcastIntervalMs`, and `broadcastJitterMs` in
`include/app_config.h` to set the message and timing. Each board waits a random
1-2000 ms at startup, then waits 10 seconds plus a random 0-1999 ms after each
send. `broadcastJitterMs` must be positive. Boards listen between sends; the
radio cannot receive while transmitting.

Messages are ASCII text without embedded NULs. A compile-time check limits the
text plus its header to 255 bytes. All radios must use identical frequency,
bandwidth, spreading factor, coding rate, sync word, preamble, and CRC settings.

Each transmission contains a random boot-session token and sequence number.
The session changes when a board restarts; the sequence increases for each
transmission attempt. Boards only display received packets beginning with
`LORA_BROADCAST:`. This prefix and the session are labels, not authentication.
CRC detects radio corruption; the payload remains readable by compatible
receivers. See the [RadioLib LR11x0 API](https://jgromes.github.io/RadioLib/class_l_r11x0.html)
for the underlying transmit and receive operations.

There are no acknowledgements or retries: `sent` counts successful local
transmissions and does not prove that another board received them. Simultaneous
broadcasts can collide. Random timing reduces repeated collisions; it does not
guarantee delivery. No board automatically forwards packets, so this is direct
broadcast rather than a mesh network. LoRa packets are small; this example is
for text or sensor values.

## Wiring on every board

Matches `lora_connection.jpg`. Pin numbers are ESP32 GPIO numbers.

| Core1121 signal | ESP32-S3 |
| --- | --- |
| MISO | GPIO 5 |
| MOSI | GPIO 6 |
| SCK | GPIO 7 |
| NSS / CS | GPIO 15 |
| RESET | GPIO 16 |
| BUSY | GPIO 17 |
| DIO9 / IRQ | GPIO 18 |
| VCC | 3.3 V |
| GND | GND |

BUSY must be moved from 39 to **17**, and IRQ from 40 to **18**, as shown
in the image. SD CMD/CLK/D0 remain on 38/39/40. This test does not initialize
the SD card. Power each radio from its own board with a shared ground between
that board and its radio. Attach the 868 MHz antenna to the module's sub-GHz
antenna connector before powering/transmitting. Start with the boards 1-2 m apart.

## Radio settings

Settings are in `include/app_config.h`; use identical radio settings on all boards:

- 868.000 MHz, bandwidth 125 kHz, SF7, coding rate 4/5.
- Private sync word 0x12, eight-symbol preamble, CRC enabled.
- 0 dBm (1 mW) output power for initial testing.
- Core1121-XF TCXO supply: 3.0 V.
- DIO5/DIO6 antenna switch: RX=1/0, TX=0/1, standby=0/0.
- Sub-GHz high-power amplifier path selected explicitly, even at 0 dBm.

The oscillator and switch settings follow the
[Waveshare Core1121-XF demo](https://files.waveshare.com/wiki/Core1121/Core1121_XF_Demo.zip),
specifically its Arduino `lr1121_config.cpp` and `lr1121_common` configuration.
RadioLib is pinned to 7.7.1. This assumes a Waveshare Core1121-HF (868 MHz)
with LR1121 **transceiver firmware**, not its separate LoRaWAN modem firmware.

For Singapore, consult the
[IMDA short-range device specification](https://www.imda.gov.sg/-/media/Imda/Files/Regulation-Licensing-and-Consultations/ICT-Standards/Telecommunication-Standards/Radio-Comms/IMDATSSRD.pdf)
for current requirements before deployment. This test uses 868 MHz and low
output power; that does not establish equipment compliance.

## Serial logging

`pio device monitor` adds computer timestamps and saves serial output
automatically through the `time` and `log2file` filters. Close and reopen the
monitors from this project directory to enable these filters.

With the installed PlatformIO 6.2.0, files are saved as
`logs/device-monitor-YYMMDD-HHMMSS.log`. Each monitor prints its exact log path.
Start the monitors at least two seconds apart: the built-in logger names files
to second precision, so opening both in the same second can overwrite/mix logs.
Other PlatformIO versions may use `platformio-device-monitor-*.log` in the
working directory. Both `logs/` and `*.log` are ignored by Git.

Logs contain the board's serial output, including broadcast packets, RSSI, SNR,
counters, and errors. Timestamps are computer receipt times, not radio airtime
measurements. Logging stops when the monitor closes and does not record to the
ESP32's SD card. Use Ctrl+C to stop each monitor.

## Expected result and hardware validation

Illustrative output on a board (actual tokens and signal readings vary):

```text
Device broadcast node (send and receive)
Radio ready: 868.000 MHz | BW 125 kHz | SF7 | CR4/5 | 0 dBm
TX LORA_BROADCAST:abcd1234:1:Hello from ESP32!
Stats: sent=1 tx_errors=0 (no replies requested)
RX [43 bytes] LORA_BROADCAST:bcde2345:1:Hello from ESP32! | RSSI -55.0 dBm | SNR 9.0 dB
Broadcasts received=1
```

Hardware validation is pending. Flash the same firmware to at least two boards
and confirm each prints both its own TX and the other board's RX packets.
With three or more boards, check that multiple listeners display the same
sender session and sequence. Check sequence transitions 9 to 10 and 99 to 100,
then restart a board to verify its session changes. Turning one board off must
not stop the remaining boards from broadcasting and receiving.

## Troubleshooting

- **`ar.exe ... .cpp.o: No such file or directory`:** the archive step is
  missing an intermediate build object. Wait for any build or upload to finish,
  then run `pio run --target clean` followed by `pio run`, or use **Clean** then
  **Build** in Project Tasks. Run build and clean tasks one at a time.
- **Initialization error:** check power, ground, all SPI wires, RESET, BUSY,
  IRQ, and the reported RadioLib error code. A successful build alone does
  not establish SPI communication. Firmware must be the LR1121 transceiver
  variant; this program does not update radio firmware.
  The firmware repeats the failed operation and RadioLib code every five
  seconds while stopped, so the error remains visible if the monitor opens
  after startup. Rebuild and upload to use this diagnostic output.
- **TX error:** check IRQ on GPIO18 and BUSY on GPIO17.
- **Radio ready but no received broadcasts:** verify at least two boards are
  running, identical radio settings, correct sub-GHz antenna ports, and 868 MHz
  modules/antennas. Check that each board's monitor shows its own TX packets.
- **CRC errors / intermittent reception:** check stable 3.3 V power, shorten
  jumper wires, move the boards farther apart if very close, and inspect antennas.
- **No serial output:** check `ARDUINO_USB_CDC_ON_BOOT` matches your USB
  connector and select the correct port; reset with the monitor open.

This is intentionally a blocking, small bench-test program. Receive calls have
bounded timeouts; integrate nonblocking radio handling before adding concurrent
audio, SD streaming, or other timing-sensitive work.

The receive helper uses a zero-filled byte buffer for these ASCII packets.
This avoids RadioLib 7.7.1's String receive path querying the LR11x0 packet
length after its RX buffer has been cleared, which can produce empty text
despite successful reception. Reflash every board when applying this fix.

Before each receive, the helper also restores explicit-header mode with a
255-byte receive limit. RadioLib 7.7.1 leaves the last TX length in the chip's
packet parameters when switching to explicit RX. That length then limits
incoming packets: without this workaround, a board can reject a longer incoming
packet after transmitting a shorter one. Verify reception continues across
sequence transitions 9 to 10 and 99 to 100 after reflashing.
