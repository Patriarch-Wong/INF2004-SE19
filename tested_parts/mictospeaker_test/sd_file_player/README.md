# SD-card MP3 playback test

Standalone SD MP3 player for the speaker ESP32-S3 N16R8. It reuses the earlier
working `speaker_test` player and official Espressif MP3 decoder. Wi-Fi and the
microphone are not used. The parent project's ESP-NOW firmware is preserved.

Input: any audio file you have the rights to use, saved as `../track.flac`
(in `mictospeaker_test`). The tested input was a 44.1 kHz stereo, 4:06
track. Encoded file: `track.mp3`, 44.1 kHz stereo, 192 kbit/s, 5,910,151
bytes for the tested track. Audio files are ignored by Git. Playback mixes
to mono at unity gain (100%, no software volume attenuation) on GPIO 7.

SD wiring: CLK 39, CMD 38, D0 40, using 1-bit SDMMC. Speaker signal stays on
GPIO 7 through the existing resistor/capacitor filter. The card must already
have a supported FAT filesystem; the player never formats it.

From the parent `mictospeaker_test` directory, with the speaker monitor closed:

```sh
pio run -d sd_file_player -e sd_speaker -t upload --upload-port PORT
python3 sd_file_player/scripts/send_mp3.py --reset --port PORT sd_file_player/track.mp3
```

The `--reset` option resets the UART/COM-connected board before transfer. The
script transfers acknowledged 4096-byte chunks, checks the card's readback
SHA-256, then starts playback and monitors it to completion. It saves a staging
file before renaming to `/track.mp3`. A matching existing file
is reused; a different existing file is preserved and rejected. Other SD files,
including the earlier MP3 test track, are preserved.

Add `--start-only` to release the serial port once successful playback progress
is reported; the board continues playing independently.

Expected SHA-256 for the tested track:
`673d7e3b8c579034ede7ac5038d01ed6e7be227d408d80d3a3d1e9da77ed8211`

For later playback, open the speaker monitor at **921600 baud**:

```sh
pio device monitor -d sd_file_player -e sd_speaker --port PORT
```

Send `play` then Enter to replay, or `s` then Enter during playback to stop.
Playback does not start automatically after reset. To restore microphone
streaming, reflash the parent's `speaker_rx` environment.

Re-create the MP3 (the command refuses to overwrite an existing output):

```sh
ffmpeg -hide_banner -nostdin -n -i track.flac -map 0:a:0 -map_metadata -1 -c:a libmp3lame -b:a 192k -ar 44100 -ac 2 -write_xing 0 -id3v2_version 0 sd_file_player/track.mp3
```

Codec binary/headers and license are in `lib/esp_audio_codec`; provenance is
recorded there. No additional decoder libraries are downloaded.
