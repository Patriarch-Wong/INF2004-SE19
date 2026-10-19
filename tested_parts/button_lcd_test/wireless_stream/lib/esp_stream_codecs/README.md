# Pinned ESP32-S3 decoder dependencies

This directory vendors only the decoder integration needed by the Arduino
PlatformIO build. No dependency download runs at build time for these codecs.

H.264: Espressif `esp_h264` 1.4.1 from
[`afff884d9fa6674658faaf6b2003e7969be287ef`](https://github.com/espressif/esp-h264-component/tree/afff884d9fa6674658faaf6b2003e7969be287ef/esp_h264).
Included: decoder interface/parameter/software C sources, the allocation
port for ESP-IDF below 5.3, headers, and ESP32-S3 `libtinyh264.a`.
The encoder archive is omitted. See `LICENSE.h264` and header notices.

AAC: Espressif `esp_audio_codec` 2.6.2 from
[`a1c9747e62e9f78444570ce6dfb0c4143de98393`](https://github.com/espressif/esp-adf-libs/tree/a1c9747e62e9f78444570ce6dfb0c4143de98393/esp_audio_codec).
Included: public decoder headers and ESP32-S3 `libesp_audio_codec.a`.
The player calls the AAC-specific API; unused codec objects are not linked.
See `LICENSE.audio` and header notices; this component is licensed for use
with Espressif products.

Upstream sources and binaries are unmodified. `library.json` and `link.py`
provide the PlatformIO adapter. The H.264 decoder uses one task; the AAC
playback worker runs separately. Both packages declare ESP-IDF >= 4.4.
