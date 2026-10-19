# ICS43434 → ESP-NOW → YwRobot speaker

Two **ESP32-S3 N16R8** boards stream live microphone audio without a router:

```text
ICS43434 → GPIO 4/5/6 → mic ESP32 → ESP-NOW → speaker ESP32 → GPIO 7 → YwRobot S
```

The receiver reuses the **I2S PDM output on GPIO 7** from the sibling
`../speaker_test/src/main.cpp` project, including its initial **15% output gain**.
No SD card, MP3 decoder, external I2S amplifier, or extra libraries are required.

## Wiring

Microphone board:

| ICS43434 pin | ESP32-S3 connection |
| --- | --- |
| SCK / BCLK | GPIO 4 |
| WS / LRCLK | GPIO 5 |
| SD | GPIO 6 |
| L/R | GND (left channel) |
| VDD | 3.3 V |
| GND | GND |

Speaker board:

| YwRobot module pin | Connection |
| --- | --- |
| S / signal | GPIO 7 |
| VCC / + | Supply matching the module's voltage marking |
| GND / − | Speaker ESP32 GND |

GPIO 7 drives the powered module's signal input, not a bare speaker. As in
`speaker_test`, PDM playback quality depends on the module's input filtering;
a low-pass filter or external audio DAC may be needed if it sounds noisy.
Keep the speaker away from the microphone to avoid acoustic feedback.

## Build and flash

Run commands from this project directory, not the separate starter in `test/`.
The platform remains pinned to Espressif32 7.1.3 / Arduino-ESP32 2.0.17.

```sh
# Compile both UART/COM firmwares.
pio run

# Identify the two USB ports.
pio device list

# Upload with explicit CLI ports.
pio run -e mic_tx -t upload --upload-port MIC_PORT
pio run -e speaker_rx -t upload --upload-port SPEAKER_PORT

# Monitor either board at 115200 baud.
pio device monitor -e mic_tx --port MIC_PORT
pio device monitor -e speaker_rx --port SPEAKER_PORT
```

Replace `MIC_PORT` and `SPEAKER_PORT` with the ports from `pio device list`;
no ports are saved in `platformio.ini`. Use separate terminals to monitor
both boards.

If using the native USB connector, use `mic_tx_usb` and `speaker_rx_usb` instead,
for both uploads and serial monitoring, and specify its port explicitly; native
USB ports differ from the UART ports above. Close serial monitors before uploading.
If download mode fails, hold BOOT, tap RESET, release BOOT, then retry.

Power both boards: the microphone sends automatically and the speaker plays
automatically. The transmitter targets the speaker station MAC
`E0:72:A1:D7:F6:50`, recorded in the saved speaker log. If the speaker board is
replaced, update `receiverMac` with its printed station MAC before flashing the mic.
Flash each board with its own role; flashing the same role to both will not work.
**Reflash both boards for AUD2 ADPCM.** The previous AUD1 PCM firmware is
incompatible and is deliberately rejected. The ADPCM firmware has been
compile-checked and tested on both boards. We heard noticeably better
live speech after unicast and the 20 ms send-wait change; clarity is still being
evaluated.

## Settings and diagnostics

Edit `include/app_config.h`:

- `micGain`: input gain, initially 1 (unity). Reduce if the microphone peak stays near
  32768; increase if speech is too quiet. Values 1–64 are supported.
- `speakerVolumePercent`: output gain, initially 15 to match `speaker_test`.
- `speakerUpsampleFactor`: initially 3, for 48 kHz speaker playback of the 16 kHz
  stream. Set to 1 to compare against the original 16 kHz output; reflash only
  the speaker after both boards have AUD2. We heard a clearer local tone
  and less hiss at 48 kHz; subsequent ADPCM/unicast testing also improved live audio.
- `wifiChannel`: initially 6; both boards must use the same channel.
- `radio_send_wait_ms`: 20 ms maximum wait for the preceding send callback. A live
  unicast diagnostic with the previous 8 ms wait dropped 22 of 251 blocks in
  five seconds before encoding. This change allows more retry time; compare
  `busy-drop` deltas and RX loss/underruns to check whether it helps. Also check
  that `DMA-overflows` remains zero and about 250 blocks are submitted every
  five seconds: fewer busy drops alone do not prove recording is keeping up.
- `receiverMac`: `E0:72:A1:D7:F6:50` for unicast to the recorded speaker board.
  Unicast enables MAC acknowledgements/retries to reduce packet loss. Copy a
  replacement speaker's printed **station MAC** here and rebuild the microphone
  firmware; use all `0xff` bytes to restore broadcast.
- `transmitterMac`: optional receiver filter for the microphone's station MAC.
  All zeros accepts the first active sender, with a two-second source timeout.
- Pins and `micLeftChannel`: already set to the confirmed wiring above.

The link is unencrypted. Unicast send completion confirms MAC-layer delivery,
not successful audio playback; use receiver loss/underrun counters and listen to
the radio test tone to verify improvement. In broadcast mode, send completion
confirms radio transmission, not that the speaker received it.
A MAC filter or unicast destination alone does not add encryption.

Every five seconds, the microphone prints sent/completed/failed packet counts,
busy drops, capture errors, DMA overflows, recent PCM peak/RMS, clipped samples, and raw left/right slot peaks. The speaker prints received,
missing/stale packets, queue drops, underruns, concealed gaps, I2S errors, and buffered packets.

- **RX packets stays at zero:** check roles, power, channel, distance, and any MAC
  settings. Both boards print their role and station MAC at boot.
- **RX packets grows but no sound:** check speaker power, S on GPIO 7, common
  ground with its board, volume, and the transmitter's peak level.
- **Mic peak stays zero:** check 3.3 V, GPIO 4/5/6 order, and L/R tied to GND.
- **Distortion:** reduce microphone gain or speaker volume; separate the boards
  acoustically. Missing packets/underruns suggest radio interference or range.

## Diagnose static

The saved ADPCM logs show 146 missing packets and 108 underruns in about 55
seconds, alongside occasional full-scale microphone samples. The transmitter
now uses unicast to test whether MAC acknowledgements/retries reduce the radio
loss. Reflash only the microphone for this change if both boards already run
AUD2. With unicast and the 20 ms wait, the latest supplied TX readings show
1505 submitted blocks and only one additional busy drop over roughly 30 seconds,
with no send failures, DMA overflows, I2S errors, or clipped samples in those
readings. Audio sounded noticeably better. Fresh RX statistics are
still needed to quantify receiver loss/underruns. Compare `t` on the mic (radio tone)
against `a` (real mic): clean radio tone with crackling speech points back to
capture rather than packet delivery.

For the earlier AUD1-to-AUD2 upgrade, reflash **both boards**. The latest
unicast/send-wait change only requires reflashing the mic. Close monitors first.
Then open the speaker monitor using its explicit CLI port above and send:

- `t`: continuous local 500 Hz test tone, bypassing microphone and radio audio.
- `s`: digital silence through the same PDM output.
- `m`: stop PDM, disconnect it from GPIO 7, and hold the signal low.
- `a`: return to live microphone audio.

Press Enter after the letter. Keep the module's physical volume low for testing.
If the local tone is clean but live audio is noisy, inspect TX `Lpeak`, `Rpeak`,
`rms`, and `clipped` and the RX loss counters. The confirmed L/R-to-GND wiring
selects the left slot; its peak should respond to speech. Repeated full-scale
values at unity gain suggest a capture/wiring problem or a very loud input.
If static remains in `s` mode, transmitted microphone samples and missing audio
packets are excluded as its direct source: inspect PDM input filtering, power,
ground, and the speaker module. This test does not disable Wi-Fi activity.

Compare speaker `s` against `m`:

- Hiss in `s` but quiet in `m`: noise depends on the active PDM output. Check the
  analog input's low-pass filtering and output configuration. A software audio
  compression codec cannot remove switching noise generated after decoding.
- Hiss in both: investigate amplifier gain/noise, supply, ground, and RF pickup.
  Wi-Fi remains active in both modes, so this comparison does not isolate RF noise.
- `t`, `s`, or `a` reconnects PDM and resumes playback after `m`. Switching modes
  may produce a click; judge the steady sound after the transition.

The firmware captures signed PCM16, transports it as ADPCM, and decodes it back
to PCM before hardware PCM-to-PDM conversion. Espressif specifies a low-pass filter when connecting PDM DAC output
to an analog power amplifier; the module's input filtering has not been verified.
See [Espressif PDM TX line modes](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html#pdm-tx-mode).

For the steady hiss, if GPIO 7 currently connects straight to the powered
module's S input without an external filter, a first hardware experiment is:

```text
GPIO 7 ---- 1 kOhm ----+---- speaker module S
                      |
                     33 nF
                      |
                 common GND
```

Power off before rewiring. This is a single-pole RC low-pass with an unloaded
cutoff of about 4.8 kHz; the module's input impedance can alter the response.
It attenuates PDM switching content and also softens the highest audio frequencies.
These values are a starting experiment, not a measured fix for this module.
The current user setup has only the 1 kOhm series resistor, without the 33 nF
capacitor; the resistor alone is not this RC filter.
Compare `s` and `t` on the speaker before/after at the same physical volume.
Digital filtering or a microphone noise gate cannot remove hiss that remains
when the speaker locally generates digital silence. If one RC stage is
insufficient, the output circuit needs further investigation.

We heard clearer MP3 playback on the same wiring, hiss during local tone
and digital silence, and no hiss with PDM stopped. That isolates a dependence on
active PDM but does not prove the external filter is the sole cause. The receiver
uses 48 kHz playback through 3x linear interpolation; microphone capture stays
at 16 kHz. This gave a clearer tone and less hiss.
The earlier MP3 track's exact sample rate has not been measured. The pinned PDM
driver changes its internal interpolation ratio with the PCM sample rate, so
this test changes that ratio from 6 to 2 while retaining the nominal 6.144 MHz
PDM clock. No clock-divider override was added.

Before the unicast/send-wait update, live audio was broken/crackling, with RX logs
showing about 14% sequence gaps and TX logs showing busy drops. AUD2 reduces
packet frequency and bandwidth to test whether this improves continuity. It
does not denoise the microphone or guarantee radio delivery.

After reflashing **both boards**, confirm `ADPCM` on both and
`playback=48000 Hz, radio=16000 Hz` on the speaker. Send `a` on the speaker and
`t` on the **mic** monitor: this sends a synthetic 500 Hz tone through the actual
encoder, radio, decoder, and playback path. Mic DMA still clocks the sender.

- Radio tone clean, microphone speech noisy: focus on capture, mic gain/wiring,
  or acoustic feedback. Send `a` on the mic to restore capture.
- Local speaker tone clean, radio tone broken: inspect consecutive TX busy-drop
  and RX missing/underrun counters. Test close range before changing gain.
- `s` on the mic sends silence through the radio. `s` on the speaker bypasses
  radio audio. Always distinguish which monitor receives a command.

TX peak/RMS/clipping statistics describe the selected source; Lpeak/Rpeak still
describe the physical microphone. A remaining noisy local speaker tone still
needs output investigation; packet loss cannot cause noise in that local test.

## Audio transport

Audio is 16 kHz mono PCM16 in memory, encoded as 4-bit IMA ADPCM on the radio:
64 kbit/s codec data. Each 180-byte packet contains 320 samples (20 ms) as 160
ADPCM bytes plus a 20-byte header. The header includes AUD2 signature, boot
session ID, sequence, sample rate/count, initial predictor, step index, and a
reserved byte. Nibbles are high-first. Every packet carries its own decoder
state, so lost packets or sender restarts cannot desynchronize later decoding.
This is a custom packet container, not a WAV file.

At 50 packets/s this uses 72 kbit/s including our headers, before Wi-Fi overhead,
compared with roughly 274 kbit/s and 138 packets/s for the previous PCM transport.
The lossy codec trades some fidelity for reduced radio load; it cannot repair
bad microphone samples. Old AUD1 packets, malformed lengths/headers, invalid
step indices, and unsupported rates/counts are rejected before decoding.

The microphone captures both 32-bit I2S slots, selects the left slot in software,
and scales/clamps to PCM16. SD has an internal pull-down for inactive slots.
ESP-NOW uses a 6 Mbps PHY rate to reduce airtime. The sender permits one send in
flight and waits up to 20 ms for completion before dropping a block; microphone
DMA continues capturing during this bounded wait. The receiver validates packets in the Wi-Fi
callback and queues compressed blocks; decoding and playback run separately in
the Arduino loop through I2S DMA. It starts with three queued packets (60 ms),
keeps at most six (120 ms), and drops oldest
queued data when full, rejects duplicate/out-of-order sequence numbers, and
handles a transmitter reboot using the session ID. Short gaps are filled with
a fade to silence and a fade back into received audio; concealment is bounded
by available queue headroom. Queue starvation outputs silence and buffers again.
Unicast uses the radio's MAC-layer retries; no application-layer retransmission
or clock-drift resampling is implemented. Actual latency and audio quality require
hardware measurement.

## Checks

```sh
pio run -e mic_tx -e speaker_rx -e mic_tx_usb -e speaker_rx_usb
c++ -std=c++11 -Wall -Wextra -Werror -Iinclude checks/audio_packet_test.cpp -o /tmp/mictospeaker_packet_test
/tmp/mictospeaker_packet_test
c++ -std=c++11 -Wall -Wextra -Werror -Iinclude checks/adpcm_reference_driver.cpp -o /tmp/adpcm_reference_driver
# Requires Python 3.9-3.12 with audioop; the Mac system Python has it.
/usr/bin/python3 checks/adpcm_reference_test.py /tmp/adpcm_reference_driver
```

The host check covers invalid packet rejection, decoding after skipped blocks,
sequence wraparound/duplicate rejection, gain clipping, signed fades, and bounded
gap concealment, signed 3x interpolation, packet-boundary continuity, silence,
and bypass playback. The independent audioop comparison checks encoder bytes,
decoded PCM, and final state for 356 blocks spanning all 89 step indices,
silence, a sine wave, full-scale transitions, and random samples. These checks do not simulate
ESP-NOW timing or the microphone/speaker hardware.

## References

- [Espressif ESP-NOW, IDF 4.4.7](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/network/esp_now.html)
- [Espressif I2S and PDM, IDF 4.4.7](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/peripherals/i2s.html)
- [TDK ICS43434](https://uat.invensense.com/en-us/products/microphone/ics-43434)
