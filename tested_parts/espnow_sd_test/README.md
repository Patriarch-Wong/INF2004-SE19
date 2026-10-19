# ESP-NOW to SD: one-way test

Board 1 generates 4096 bytes in RAM and sends them to board 2 over ESP-NOW.
Board 2 saves the data to SD, closes and reopens the file, checks its size,
every byte, and CRC32, then acknowledges completion. Only board 2 needs SD.
Both boards run the same firmware; their station MACs determine their roles.

| Board | Role | Station MAC |
| --- | --- | --- |
| 1 | RAM sender | E0:72:A1:D9:A4:94 |
| 2 | SD receiver | E0:72:A1:D8:E2:10 |

`include/espnow_config.h` selects channel 1, both allowed MACs, and sender
index 0. SD uses CMD 38, CLK 39, D0 40, 1-bit SDMMC at 20 MHz. Settings are
in `include/app_config.h`. The receiver needs a FAT-formatted card; the
installed framework does not support exFAT. Firmware never formats cards.

## Current hardware status

Hardware verified on 2026-09-22 with the one-way firmware uploaded to both
boards. Board 1 sent 4096 bytes from RAM; board 2 saved and read back all
4096 bytes with CRC32 `f3cc9f13`. Both reported PASS and zero receive-queue
drops. Capture: `serial_one_way.log`. Receiver file on SD:
`/espnow_sd_test/rx_bec55134.bin`.

Before upload, board 2 could create/write/reopen an SD file while board 1's
card failed to mount (`serial_card_check.log`). Board 1's card is not used
by the one-way firmware. One complete transfer has been verified; this does
not establish long-run reliability or resolve the unused card's failure.

## Build and upload

```sh
pio run -e esp32-s3-n16r8
```

On each board, hold BOOT, press and release RESET, then release BOOT before
uploading. Close serial monitors first.

```sh
pio run -e esp32-s3-n16r8 -t upload --upload-port BOARD1_PORT
pio run -e esp32-s3-n16r8 -t upload --upload-port BOARD2_PORT
```

Upload BOTH boards: the one-way protocol has a different signature from the
old bidirectional firmware. Replace `BOARD1_PORT` / `BOARD2_PORT`
with the ports from `pio device list`.
The default environment uses UART with monitor DTR/RTS disabled. Native USB
users can select `esp32-s3-n16r8-usb` and its corresponding ports instead.

## Run and verify

The Python script requires pyserial. Pass the RECEIVER port first, then the
SENDER port:

```sh
python3 scripts/check_serial.py --ports BOARD2_PORT BOARD1_PORT
```

Reset both boards without holding BOOT. The checker captures `serial_test.log`
and requires both of these statuses from the correct boards:

```text
Status | receiver | RX PASS (4096/4096) | drops 0
Status | sender | TX PASS | drops 0
```

Alternatively, open one terminal per board and monitor them directly:

```sh
pio device monitor --port BOARD1_PORT --baud 115200 --dtr 0 --rts 0
pio device monitor --port BOARD2_PORT --baud 115200 --dtr 0 --rts 0
```

Press RESET on both boards without BOOT; look for sender `TX PASS` and
receiver `RX PASS`. Press Ctrl+C to close each monitor. No reupload is
needed to repeat the test. Serial logs and SD binary files are local
artifacts excluded from Git; the measured result is recorded above.

RX PASS confirms SD write, close, reopen, and read-back verification. TX PASS
confirms the receiver acknowledged that verification. Data flows one way;
acknowledgments return over ESP-NOW. The sender never mounts or accesses SD.

Data travels in 192-byte chunks inside 216-byte packets. Each packet allows
up to 60 attempts, one second apart. Duplicate requests are acknowledged
without writing data twice. SD I/O runs in the main loop, outside callbacks.
Packets are unencrypted; the MAC whitelist filters traffic, not authentication.

Each receiver run exclusively creates `/espnow_sd_test/rx_<session>.bin`.
Existing files are preserved. A failed run can leave a partial file; repeated
runs consume space. Random filename collisions fail safely. Reset BOTH boards
to repeat, including after timeout or a reboot of just one board.

## Host checks and limits

```sh
c++ -std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -I include tests/protocol_test.cpp -o /tmp/espnow_sd_protocol_test
/tmp/espnow_sd_protocol_test
```

Host checks passed for the standard CRC32 vector, chunk boundaries including
the short last chunk, and corruption detection. Hardware results are above.
This is a fixed-size functional test, not an arbitrary-file service or a
throughput benchmark. It does not test persistence through power loss.
