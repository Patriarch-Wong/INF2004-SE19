# Diagnostic snapshots — 2026-09-23

## Unicast and 20 ms send wait

We captured `20260923-unicast-20ms-mic.txt` after flashing the updated
microphone firmware and heard noticeably better audio. Across its six report
intervals (roughly 30 seconds), 1505 packets were submitted with one additional
busy drop. Send failures, I2S errors, DMA overflows, and interval clipping counts
were zero. Before increasing the wait from 8 ms, a live unicast reading showed
22 busy drops among 251 blocks in one interval.

The current output wiring includes a 1 kOhm series resistor, without the
proposed 33 nF capacitor. This is not the documented RC low-pass filter.
No matching new RX log was supplied, so these results do not quantify current
receiver loss or underruns. The ADPCM encoder/decoder matched Python audioop
for all 356 reference cases; the codec was not changed.

## Earlier broadcast baseline

The two text logs contain a simultaneous 60-second capture at 115200 baud.
Both boards booted with their correct roles, AUD2 ADPCM, and Wi-Fi channel 6.
Speaker playback was 48 kHz from a 16 kHz radio stream.

The last periodic report, approximately 55 seconds after boot, recorded:

- TX: 2721 submitted, 24 busy drops, zero radio failure reports or I2S errors.
- RX: 2599 received, 146 missing, 108 underruns, zero I2S errors.
- Across logged intervals, 226 of 878400 microphone samples reached full scale.

Listening observations after the capture:

- The synthetic tone sent from the microphone board was mostly clean with
  occasional interruptions.
- Live microphone speech was audible but overwhelmed by hiss/blowing noise.
- Sending digital silence from the microphone board removed that hiss.
- A 4 kOhm resistor reduced noise; its location has not been confirmed.

These results make microphone capture worth investigating but do not establish
whether the cause is the microphone, wiring, acoustic feedback, timing, or
sample interpretation. The actual microphone model remains unconfirmed.

Raw voice recordings, music files, build products, and machine-generated editor
files remain in the original local project and are not included here.
The SD player requires a locally supplied MP3; its bundled codec is preserved
with the upstream license and provenance.
