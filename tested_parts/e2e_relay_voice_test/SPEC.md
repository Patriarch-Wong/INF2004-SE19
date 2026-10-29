# Two-way Opus voice through a relay: specification

## Question

Can both paired handsets send and receive Opus voice end to end, with every
frame crossing a relay that cannot read it, one direction at a time and both
directions at once?

## Setup

- Boards 1 and 2: handsets (`board1`, `board2`). Already paired in stage 4
  (pair key `48FF15EF` in NVS). Each one both sends and receives.
- Board 3: relay (`board3-relay`). Network key only: it checks the hop tag,
  reads the header and forwards the frame unchanged. It has no pair key,
  NVS, session or codec code.
- Channel 11, Wi-Fi power save off. Baseline runs use the default 1 Mbps;
  successful run 3 sets 6 Mbps on all three boards with `radio 11 6` before
  voice starts. Stage 6 Opus settings (16 kHz mono,
  24 kbit/s CBR, 20 ms frames, complexity 5), 60 s streams (3,000 frames),
  stage 6 test signal, 200 ms live jitter buffer.
- Every handset frame except pairing goes to the relay's MAC. A handset
  rejects any non-pairing frame that did not come from the relay's MAC.

## Run

One capture, all three boards, about 4.5 minutes:

1. 1 → 2 alone.
2. 2 → 1 alone.
3. 1 → 2 and 2 → 1 at once (two-way).

## Checks

`tools/analyse_run.py` decides each check from the three logs.

| Check | Pass |
| --- | --- |
| R1 Relay path | Every data frame a handset accepted has a relay `FWD` line with the same type, source, destination and counter; no frame was rejected as `not via the relay`; the relay prints `Pair keys held: 0` |
| R2 Sessions | Both handsets log a session with the other under the same session ID, and each accepts the other's text frames |
| R3 One way | 1 → 2 and 2 → 1 alone each pass the stream criteria |
| R4 Two way | 1 → 2 and 2 → 1 at the same time each pass the stream criteria |
| R5 Pipelines | Zero timer, radio-queue, encode, codec and play-log drops on both handsets; capture-to-send p99 under 20 ms |
| R6 Relay | Zero relay receive-queue drops and no dropped frames |

Stream criteria: at least 99% of 3,000 packets received, every received
CRC32 matches the sent packet, late + lost under 1% at 200 ms, in-order
decode correlation above 0.99 and SNR above 20 dB, `LIVE_MISSED 0`, and the
stream was not abandoned.

## Not tested

- Confidentiality on the air. There is no sniffer in this run; stages 5
  and 6 showed no Opus packet or marker in captured ciphertext, and the
  frame format is unchanged. The relay build holds no pair key.
- A relay that tampers with frames (stage 3 tested this).
- Range: the boards sit side by side, so the relay is forced by
  addressing, not by distance.
