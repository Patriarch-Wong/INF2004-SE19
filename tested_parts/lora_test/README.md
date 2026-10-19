# Two-device LoRa test: ESP32-S3 + Waveshare Core1121

Device A sends a numbered PING every 10 seconds. Device B returns the same
session/sequence in a PONG. A prints round-trip time, replies, timeouts, and
success percentage; both devices print received RSSI and SNR. This is direct
LoRa communication: no gateway, Internet connection, or LoRaWAN keys needed.

## Wiring on both devices

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
antenna connector before powering/transmitting. Start with the boards 1–2 m apart.

## Radio settings

Settings are in `include/app_config.h`; use identical radio settings on both devices:

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

For Singapore, the [IMDA short-range device specification](https://www.imda.gov.sg/-/media/Imda/Files/Regulation-Licensing-and-Consultations/ICT-Standards/Telecommunication-Standards/Radio-Comms/IMDATSSRD.pdf)
lists 866–869 MHz and notes that the allocation is under review. This test uses
868 MHz and low output power; that does not establish equipment compliance.
Check current requirements before deployment.

## Build and upload

The existing ESP32-S3 N16R8 settings are retained: 16 MB QSPI flash, 8 MB
OPI PSRAM, custom `partitions.csv`, and PlatformIO espressif32 7.1.3.

Find the two serial ports:

```sh
pio device list
```

Use the UART/COM connector and replace `PORT_A` and `PORT_B` with the actual
ports (on macOS, typically `/dev/cu.usbserial-...` or `/dev/cu.usbmodem...`).
Select the role manually near the top of `src/main.cpp`:

```cpp
#define LORA_INITIATOR 1  // 1 = A (PING sender), 0 = B (PONG responder)
```

Set it to **0**, save, and upload B:

```sh
pio run -t upload --upload-port PORT_B
```

Then set it to **1**, save, and upload A:

```sh
pio run -t upload --upload-port PORT_A
```

Open two terminals, one monitor per device:

```sh
pio device monitor --port PORT_B
pio device monitor --port PORT_A
```

Both monitors use 115200 baud. For the board's native USB connector, use
`-e esp32-s3-n16r8-usb` for both boards' uploads and monitors. Close a port's
monitor before uploading to it. If needed, hold BOOT, tap RESET, release BOOT,
then upload. Reset after opening the monitor to see startup diagnostics.

Build without uploading:

```sh
pio run
```

Both boards use the same environment. The role comes only from your manual
setting in `src/main.cpp`; remember to change it before uploading the other
board. The old `lora-a` / `lora-b` environments are commented out in
`platformio.ini`, so those environment names are no longer active.

## Serial logging

`pio device monitor` now adds computer timestamps and saves serial output
automatically through the `time` and `log2file` filters. No firmware reflash
is needed: close and reopen the monitors from this project directory.

Device A:

```sh
pio device monitor --port PORT_A
```

Device B (in another terminal):

```sh
pio device monitor --port PORT_B
```

With the installed PlatformIO 6.2.0, files are saved as
`logs/device-monitor-YYMMDD-HHMMSS.log`. Each monitor prints its exact log path.
Start the monitors at least two seconds apart: the built-in logger names files
to second precision, so opening both in the same second can overwrite/mix logs.
Other PlatformIO versions may use `platformio-device-monitor-*.log` in the
working directory. Both `logs/` and `*.log` are ignored by Git.

Logs contain the device's serial output, including PING/PONG text, RSSI, SNR,
round-trip timing, counters, and errors. Timestamps are computer receipt times,
not radio airtime measurements. Logging stops when the monitor closes and
does not record to the ESP32's SD card. Use Ctrl+C to stop each monitor.

## Expected result

Illustrative device A output (actual token, signal and timing vary):

```text
Device A (PING initiator)
Radio ready: 868.000 MHz | BW 125 kHz | SF7 | CR4/5 | 0 dBm
TX LORA_TEST:PING:abcd1234:1
RX [24 bytes] LORA_TEST:PONG:abcd1234:1 | RSSI -55.0 dBm | SNR 9.0 dB
PASS round trip 240 ms
Stats: sent=1 replies=1 timeouts=0 tx_errors=0 success=100.0%
```

Device B should print the PING followed by `TX LORA_TEST:PONG:...`.
Ten or more consecutive matching replies verify traffic in both directions.
Turning B off should cause timeouts on A; powering it back on should restore
replies automatically. The success percentage is completed round trips divided
by successfully transmitted PINGs, not a one-way packet error measurement.
Round-trip timing includes both packets' airtime and B's 100 ms turnaround delay.

## Troubleshooting

- **Initialization error:** check power, ground, all SPI wires, RESET, BUSY,
  IRQ, and the reported RadioLib error code. A successful build alone does
  not establish SPI communication. Firmware must be the LR1121 transceiver
  variant; this program does not update radio firmware.
- **TX error / timeout:** check IRQ on GPIO18 and BUSY on GPIO17.
- **Radio ready but no replies:** verify one A and one B, identical settings,
  correct sub-GHz antenna ports, and 868 MHz modules/antennas. Check B's monitor
  to distinguish missing PINGs from missing PONGs.
- **CRC errors / intermittent reception:** check stable 3.3 V power, shorten
  jumper wires, move the devices farther apart if very close, and inspect antennas.
- **No serial output:** use the environment matching your USB connector and
  correct port; reset with the monitor open.

This is intentionally a blocking, small bench-test program. Receive calls have
bounded timeouts; integrate nonblocking radio handling before adding concurrent
audio, SD streaming, or other timing-sensitive work.

The receive helper uses a zero-filled byte buffer for these ASCII packets.
This avoids RadioLib 7.7.1's String receive path querying the LR11x0 packet
length after its RX buffer has been cleared, which can produce empty text
despite successful reception. Reflash both A and B when applying this fix.

Before each receive, the helper also restores explicit-header mode with a
255-byte receive limit. RadioLib 7.7.1 leaves the last TX length in the chip's
packet parameters when switching to explicit RX. That length then limits
incoming packets: without this workaround, B rejects PING 10 after replying
to PING 9 because the sequence gained a digit. Verify replies continue across
9 to 10 after reflashing; the same fix covers 99 to 100.
