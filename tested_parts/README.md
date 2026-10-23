# ESP32 tests

Standalone ESP32 experiments, each in its own project directory.

| Project | Description | Validation |
| --- | --- | --- |
| [mictospeaker_test](mictospeaker_test/) | ESP32-S3 microphone to ESP-NOW ADPCM to PDM speaker, plus capture and SD player diagnostics | Both roles boot and stream; saved logs show packet gaps and underruns; microphone noise unresolved |
| [codec_test](codec_test/) | ESP32-S3 N16R8 Opus encode/store/decode test | UART and native USB builds passed; three hardware round trips passed over UART |
| [codec_sdtest](codec_sdtest/) | ESP32-S3 N16R8 Opus + onboard SDMMC test with reusable C++ APIs | UART and native USB builds passed; three codec/SD hardware round trips passed |
| [lora_test](lora_test/) | ESP32-S3 N16R8 + Core1121 868 MHz two-device PING/PONG test with manual A/B role selection | Hardware round trips confirmed; latest receive-length fix builds successfully, hardware retest pending |
| [espnow_test](espnow_test/) | ESP32-S3 N16R8 two-board ESP-NOW test with a shared MAC whitelist | Two-way reception of sequences 1–7 confirmed; latest serial diagnostic changes build successfully; unknown-device rejection not tested |
| [espnow_encrypt_test](espnow_encrypt_test/) | ESP32-S3 N16R8 two-board ESP-NOW test with CCMP encryption, plus wrong-key and plaintext negative-test builds | All four environments build; encrypted exchange and negative tests not yet run on hardware |
| [espnow_sd_test](espnow_sd_test/) | ESP32-S3 one-way RAM to ESP-NOW to SDMMC transfer | 4096-byte hardware transfer passed with SD read-back, CRC32 verification, and zero receive-queue drops |

Open the project directory in PlatformIO, or change into it before running commands:

```sh
cd codec_test
pio run
```

See each project README for hardware requirements, upload instructions, and measured results.
