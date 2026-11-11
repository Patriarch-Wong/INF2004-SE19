# ESP-NOW live camera viewer

Flash this sketch to the second ESP32-S3 N16R8 display board. It detects the
plain or encrypted feed on Wi-Fi channel 1 and draws the latest complete
frame on a 240 × 240 ST7789. It supports `espcam_espnow_live_test`,
`espcam_espnow_live_encrypted_test`, and `espcam_espnow_video_test`.
Keep the original camera and display powered so their existing unicast
link can acknowledge packets normally. The viewer is receive-only and
uses its factory MAC address.

## Upload with Arduino IDE

1. Extract the zip. Open `ESPNowCameraViewer.ino` inside the
   `ESPNowCameraViewer` folder. Keep the other files beside it.
2. In Boards Manager, install **esp32 by Espressif Systems**. Use version
   **3.3.12**. In Library Manager, install **Adafruit ST7735 and ST7789 Library**
   with its dependencies.
3. Match the LCD wiring below. Plug the second viewer into your computer via
   its USB-to-UART connector and select its port in Arduino IDE.
4. Set these options in **Tools**, then click **Upload**.

| Setting | Value |
| --- | --- |
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB (128Mb) |
| Flash Mode | QIO 80MHz |
| PSRAM | OPI PSRAM |
| Partition Scheme | Custom |
| USB Mode | Hardware CDC and JTAG |
| USB CDC On Boot | Disabled |
| Upload Mode | UART0 / Hardware CDC |
| Upload Speed | 115200 |

If upload stalls at `Connecting`, hold **BOOT**, tap **RESET**, then release
**BOOT**. Retry the upload and tap **RESET** once it finishes.

## LCD wiring

| ST7789 pin | ESP32-S3 N16R8 |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SCL / SCK | GPIO12 |
| SDA / MOSI | GPIO11 |
| RES / RST | GPIO10 |
| DC | GPIO9 |
| BLK | GPIO14 |

This matches the original display with no CS pin.

## Watch the feed

Power up the original display and the second viewer, then power up or reset
the camera. The second screen starts at **Waiting for camera**. If the feed
is encrypted and the viewer missed the handshake, it shows **Encrypted
camera** with **Reset camera to sync**. Reset the camera with both displays
on so the viewer can hear the new handshake and open the video packets.
During encrypted playback, **Encrypted** appears at the bottom left.

The network key and the board 1–2 pair key in `camera_keys.h` are the
committed project test keys. If the camera link uses different keys, enter
the matching values in your local copy and upload it to the viewer. A
handshake that fails authentication shows **Key mismatch**.

For the boards in the repository,
the camera's Wi-Fi MAC is `E0:72:A1:D7:F6:50` and the original display's Wi-Fi
MAC is `E0:72:A1:D8:F6:20`. If your working camera link uses different
addresses or a different channel, match them in `network_config.h` before
uploading to the second viewer.

Open Serial Monitor at **921600 baud** for frame counts and queue drops. Send
`s` for the last frame sequence, `b` to swap pixel byte order, `i` to toggle
inversion, or `f` to cycle the LCD SPI speed. The second viewer skips frames
when it misses a fragment, so keep it within good reception range of the camera.

## Troubleshooting

The viewer initializes the LCD before Wi-Fi and the frame buffers. If it
shows **PSRAM failed**, set **OPI PSRAM** in Arduino IDE. **Wi-Fi failed**
shows the failing radio step on the LCD and in Serial Monitor.

The viewer prints a `RADIO` line after each `RX` line. It shows the actual
channel and counts for Wi-Fi management packets (`mgmt`), action packets
from the configured camera (`camera`), those addressed to the original
display (`link`), and recognized plain or encrypted video packets (`video`).
The first payload byte is `0x56` for plain video and `0x01` for encrypted
packets. If the count for `camera`
stays at zero with the camera running, check the camera's MAC and channel.
If `camera` rises and `link` stays at zero, check the original display's MAC.

On the unencrypted camera build, send `r` in the camera's Serial Monitor
until it reports `Radio rate 1 Mbps: ok` to test whether the viewer can hear
the stream at the lower rate. Reset the camera to restore its default rate.
