# Standalone I2S microphone capture

The microphone module's exact model is unknown. This diagnostic tests standard
Philips I2S with two 32-bit slots at 16 kHz or 48 kHz. It uses the parent project's
pin mapping: BCLK/SCK 4, WS/LRCLK 5, SD 6. L/R is grounded, and power is 3.3 V
with a common ground. No Wi-Fi, ADPCM, speaker output, gain, or audio processing
runs on the ESP32 during recording.

The board records three seconds of both raw slots into PSRAM, stops I2S, then
transfers the data over serial with SHA-256 verification. This avoids serial
bandwidth disturbing microphone capture. Startup samples are discarded, and
reported DMA overflows/read errors invalidate a capture.

From `mictospeaker_test`, close the **mic** serial monitor and manually flash:

```sh
pio run -d mic_capture -e mic_capture -t upload --upload-port PORT
```

Keep the speaker quiet to prevent acoustic feedback. Run this and speak near
the microphone when prompted (do not run a serial monitor simultaneously):

```sh
python3 mic_capture/scripts/record.py --reset --port PORT --rate 16000
```

Repeat with `--rate 48000` to compare capture modes. A timestamped folder under
`mic_capture/captures/` contains:

- `raw-i2s.bin`: untouched little-endian stereo 32-bit I2S words.
- `stereo-unity.wav`: the high 16 bits of both slots, with no gain or filtering.
- `left-listen.wav`, `right-listen.wav`: separate channels with DC removed and
  fixed peak normalization (maximum 64x), for easier listening. Normalization
  amplifies noise too; it is not denoising. An all-zero channel stays zero.
- `stats.json`: original PCM16 levels, DC, AC RMS, full-scale/nonzero counts,
  listening gain, first raw words, rate, and checksum.

Interpretation: a clear recording means the microphone can capture intelligible
audio independently of the radio/speaker. Garbage on both slots, especially the
inactive slot, calls for checking power, ground, signal routing, module identity,
and I2S timing; it is not proof of a defective microphone. With L/R low, slot 0
is expected to contain left-channel audio for this ESP32-S3 configuration.

Breadboard check: verify split power rails, use short jumpers, and connect mic
GND directly to ESP32 GND if practical. BCLK/WS/SD should connect directly to
GPIO 4/5/6, without the analog speaker's resistor/capacitor filter. Keep the
microphone's sound inlet unobstructed. Exact pull-down and bypass requirements
depend on the module: the ICS-43434 datasheet specifies an SD-to-GND 100 kΩ
pull-down (10 kΩ for faster discharge) and 100 nF between VDD and GND close to
the microphone; do not assume the unidentified board is that part.

Reference: [TDK ICS-43434 datasheet, pages 10–15](https://product.tdk.com/system/files/dam/doc/product/sw_piezo/mic/mems-mic/data_sheet/ds-000069-ics-43434-v1.2.pdf).

To restore live transmission, flash the parent's `mic_tx` environment. The
speaker firmware and SD card are not changed by this diagnostic.
