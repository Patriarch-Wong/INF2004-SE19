# Direct encrypted LoRa serial chat

Adapted from the supplied `EncryptedLoRa.ino` for **two-way** messaging between
ESP32-S3 N16R8 boards with Waveshare Core1121-HF (LR1121) radios. Type a line
in either Serial Monitor and press Enter; the other board authenticates,
decrypts, and prints it. No relay, forwarding, automatic broadcasts, Wi-Fi,
gateway, acknowledgements, or retries.

## Build and manual flash

Open this folder as the PlatformIO project. Build both variants with `pio run`.
Flash manually using the normal BOOT/RESET procedure:

| Physical board | PlatformIO environment | Sender ID | Accepts ID |
| --- | --- | --- | --- |
| A | `board_a` | 101 | 102 |
| B | `board_b` | 102 | 101 |

Both environments send **and** receive. Keep each environment on its assigned
physical board. Do not flash the same environment onto both boards.

Open one terminal per board from this directory:

```sh
pio device monitor -e board_a --port PORT_A
pio device monitor -e board_b --port PORT_B
```

Replace `PORT_A` / `PORT_B` with your ports (`pio device list`). Monitors use
**115200 baud**, local echo, and LF on Enter. Arduino Serial Monitor also works:
select **Newline**, **Carriage return**, or **Both NL & CR**. Blank lines are
ignored; CRLF sends only once. Maximum is **200 bytes** per line (UTF-8 characters
may use multiple bytes). Longer lines are rejected entirely, never truncated
and sent. The firmware buffers input while listening; sending briefly blocks.

Serial uses the UART/COM connector by default. For native USB, change
`ARDUINO_USB_CDC_ON_BOOT=0` to `1` in `platformio.ini` and rebuild both variants.
Close a monitor before flashing that board. Release BOOT and tap EN/RST after
opening the monitor if it shows `waiting for download`.

## Radio and wiring

Default frequency is **915 MHz**, matching the supplied sketch. The existing
`lora_test` uses 868 MHz. To use that frequency, change `LORA_HZ` in `src/main.cpp`
and rebuild both boards. Use matching radios/antennas and a frequency permitted
for your setup. All radio settings must agree between boards.

| Core1121 | ESP32-S3 GPIO |
| --- | --- |
| MISO | 5 |
| MOSI | 6 |
| SCK | 7 |
| NSS / CS | 15 |
| RESET | 16 |
| BUSY | 17 |
| DIO9 / IRQ | 18 |
| VCC / GND | 3.3 V / GND |

Attach the sub-GHz antenna before transmitting. Uses the existing test's 125 kHz
bandwidth, SF7, CR4/5, sync word 0x12, eight-symbol preamble, CRC, 0 dBm HP path,
3.0 V TCXO, and internal DIO5/DIO6 RF switch. RadioLib is pinned to 7.7.1,
PlatformIO espressif32 to 7.1.3. RX restores the 255-byte maximum after each TX,
and reads the binary packet length before RadioLib clears its buffer.

## Key and persistent counters

The supplied sketch's key is already installed locally in `include/secrets.h`,
which Git ignores. It is shared by both builds. Do not publish this file or the
built firmware. A fresh checkout uses `secrets.example.h` and stops at startup
until you copy it to `secrets.h` and supply a shared 64-digit hexadecimal key.
The key from the shared sketch is for this bench test; use a new private key for
any real deployment.

Packets use AES-256-GCM with a 16-byte authentication tag:
`version(1) | sender ID(4) | counter(8) | ciphertext(1..200) | tag(16)`.
Integers are big endian; the nonce is sender ID + counter, and the entire
13-byte header is authenticated. IDs 101/102 keep the nonce space separate from
ID 1 in the supplied one-way sketch. Neither side accepts that original sender.

Each board saves its TX counter **before** encrypting/sending and its RX counter
**after** authentication but **before** displaying text. Storage failures stop
the program. Replay and out-of-order packets are rejected across normal resets.
This costs one NVS write per sent/accepted message and is for occasional text.

Do not erase, restore, or clone NVS while keeping the same key and sender ID.
After erasing either board, use a new shared key on both. Keep the board roles
fixed and never run multiple physical boards with the same sender ID/key.
Normal uploads that preserve NVS retain counters. Headers, lengths and timing
are visible over radio; plaintext is intentionally visible in serial logs.
The key in firmware is not protected against physical extraction.

## Bench checks

1. Confirm A reports ID 101 and B ID 102, both at the same frequency.
2. Send `hello B` from A. B should show `RX from 101 #1: hello B` (counter may
   be higher after earlier sends). Send `hello A` from B and verify A prints it.
3. Send a short line from A, then a 200-byte line from B. Check the full long
   message arrives; repeat with the roles reversed.
4. Check blank lines do nothing, CRLF sends once, and a 201-byte line prints
   `Line too long` without sending. The next valid line should work.
5. Reset each board without erasing NVS. Verify counters continue increasing
   and messaging still works in both directions.
6. For a negative authentication check, build B temporarily with a different
   key. Messages must be rejected without plaintext output. Restore its original
   key with NVS intact; do not reset its counters.

Transmit success only means the local radio sent the packet. Both transmitting
at once can collide; take turns during this test. No message is forwarded.
Logs are stored by PlatformIO's `log2file` filter; open monitors a few seconds
apart to avoid same-second log filenames.

## Encryption checks and current debugging status

The GCM wrapper and host OpenSSL backend are reused from `e2e_security_test`,
with AES-256 (32-byte keys) in place of AES-128. No routing, relay, ESP-NOW,
pairing, or HMAC code was imported. The packet format and persistent counters
remain compatible with the first LoRa chat build.

Run the host encryption checks:

```sh
sh tests/run_crypto_tests.sh
```

These pass a fixed AES-256-GCM vector, both sender IDs, 1/16/32/200-byte
round-trips, and rejection of modified headers, ciphertext, tags and wrong keys.
The same self-test runs against the ESP32 mbedTLS backend at startup and must
print `AES-256-GCM self-test PASS` before the radio starts. Both firmware builds
pass. The user subsequently confirmed the updated test is working on hardware.
No automated passing serial capture was recorded after that confirmation;
negative authentication and replay/reset checks on hardware remain unverified.

The initial firmware was tested with both connected boards. A reported ID 101,
B reported ID 102, both at 915 MHz. Three runs failed delivery in both directions,
including a run with B sending first. A consistently stopped after its send:

```text
Restore RX maximum length failed, RadioLib code: -20
```

RadioLib defines -20 as wrong modem. B reported local TX success, but A did not
print RX even when B sent before A failed. The user subsequently reported the updated test working, but the original
radio fault's root cause was not isolated. The updated build reports TX status before RX restart (previously a restart failure
could hide it), plus temporary `[DEBUG-lora]` modem/BUSY/IRQ diagnostics. These diagnostics are retained in this bench-test build to help investigate
any recurrence of the radio fault. No radio timing or
configuration workaround has been applied without evidence.

After manually flashing the updated firmware, close both monitors and run this
reproducible hardware check using PlatformIO's Python (which includes pyserial):

```sh
~/.platformio/penv/bin/python tools/check_serial_chat.py PORT_A PORT_B
```

Use `--b-first` to test reception on A before it transmits. The script sends a
unique text line each way and exits nonzero unless each peer prints its RX line.
It does not flash or deliberately toggle reset; opening the serial bridge may
reset these boards. Ports are closed on exit. Replay/reset persistence remains
pending hardware validation after basic delivery works.
