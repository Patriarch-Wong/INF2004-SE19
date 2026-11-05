# ESP32 tests

Standalone ESP32-S3 N16R8 experiments, each in its own PlatformIO project.
Results and radio conditions are kept in one place, the
[root README's test summary](../README.md#test-summary); each project's own
README has the full procedure and measurements.

| Project | Description |
| --- | --- |
| [button_test](button_test/) | Debounced tactile button and LED |
| [dpad_lcd_test](dpad_lcd_test/) | Six debounced buttons (D-pad, confirm, cancel) shown on the ST7789 LCD |
| [dpad_menu_test](dpad_menu_test/) | Menu navigation on the ST7789 LCD with the six `dpad_lcd_test` buttons |
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
| [mic_speaker_e2e_test](mic_speaker_e2e_test/) | MAX9814 mic on one board to PWM speaker on another (mic/speaker chain from ESP32_Mic_Loopback_v4.ino), encrypted Opus with retransmission |
| [mic_speaker_e2e_test2](mic_speaker_e2e_test2/) | `mic_speaker_e2e_test` with auto-unmute: mute is debug only, any new incoming stream unmutes the speaker |
| [handset_integration_test](handset_integration_test/) | First handset integration: D-pad + LCD, connect to a pre-paired handset (request shown and authenticated), hold OK to talk (encrypted Opus over ESP-NOW), disconnect or link loss drops both sides |
| [handset_lora_integration_test](handset_lora_integration_test/) | `handset_integration_test` plus LoRa connections (choose ESP-NOW or LoRa, choose a board, accept/decline) that send only "Hello from Board N" and "Bye from Board N", encrypted; one connection at a time |
| [handset_espmesh_integration_test](handset_espmesh_integration_test/) | `handset_lora_integration_test` plus ESP-MESH connections with everything ESP-NOW has (accept/decline, push-to-talk voice, RIGHT hello), over ESP-WIFI-MESH with no router: board 1 is the preferred root and the next board up takes over if it is down; one connection at a time over any radio |
| [shared_keys](shared_keys/) | One key file (network key + pair keys for boards 1–5) shared by `handset_integration_test` and both `mic_speaker_e2e_test` projects; generator and key finder |
| [mictospeaker_test](mictospeaker_test/) | I2S microphone to ESP-NOW ADPCM to PDM speaker |
| [lcd_lora_test](lcd_lora_test/) | ST7789 LCD and Core1121 LoRa on separate SPI buses, no menu: hello/bye texts both ways at 915 MHz, radio state, counts and RSSI on screen |
| [lora_test](lora_test/) | Core1121 868 MHz LoRa broadcast send and receive |
| [lora_encrypt_test](lora_encrypt_test/) | Direct encrypted two-way LoRa serial chat; no relays |

Open the project directory in PlatformIO, or change into it before running
commands:

```sh
cd codec_test
pio run
```
