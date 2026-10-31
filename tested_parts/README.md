# ESP32 tests

Standalone ESP32-S3 N16R8 experiments, each in its own PlatformIO project.
Results and radio conditions are kept in one place, the
[root README's test summary](../README.md#test-summary); each project's own
README has the full procedure and measurements.

| Project | Description |
| --- | --- |
| [button_test](button_test/) | Debounced tactile button and LED |
| [button_lcd_test](button_lcd_test/) | ST7789 LCD video player from SD, with wireless A/V and streaming variants |
| [codec_test](codec_test/) | Opus encode/store/decode round trip |
| [codec_sdtest](codec_sdtest/) | Opus and onboard SDMMC with reusable C++ APIs |
| [espnow_test](espnow_test/) | Two-board ESP-NOW exchange with a MAC whitelist |
| [espnow_encrypt_test](espnow_encrypt_test/) | `espnow_test` with CCMP encryption, plus wrong-key and plaintext negative builds |
| [espnow_sd_test](espnow_sd_test/) | One-way RAM to ESP-NOW to SDMMC transfer |
| [e2e_security_test](e2e_security_test/) | Stages 1–2: end-to-end AES-GCM messages, replay window, over-the-air attacker |
| [e2e_relay_test](e2e_relay_test/) | Stage 3: relay forwards end-to-end messages without the pair key |
| [e2e_pairing_test](e2e_pairing_test/) | Stage 4: pairing, pair keys in NVS, per-boot session keys |
| [e2e_voice_test](e2e_voice_test/) | Stage 5: synthetic voice over the encrypted link, passive capture |
| [e2e_opus_test](e2e_opus_test/) | Stage 6: Opus voice over the encrypted link |
| [e2e_relay_voice_test](e2e_relay_voice_test/) | Two-way encrypted Opus voice between two handsets through a relay |
| [opus_broadcast_test](opus_broadcast_test/) | One-to-many Opus broadcast, no encryption |
| [e2e_broadcast_test](e2e_broadcast_test/) | One-to-many Opus broadcast encrypted with a shared group key |
| [mictospeaker_test](mictospeaker_test/) | I2S microphone to ESP-NOW ADPCM to PDM speaker |
| [lora_test](lora_test/) | Core1121 868 MHz LoRa broadcast send and receive |

Open the project directory in PlatformIO, or change into it before running
commands:

```sh
cd codec_test
pio run
```
