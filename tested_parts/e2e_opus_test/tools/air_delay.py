#!/usr/bin/env python3
"""Stage 6: where a voice frame waits, from the sniffer's air times.

For each voice frame, send->air is the sniffer's capture time minus the
sender's send time (capture time plus capture-to-send), and air->loop is the
receiver's arrival stamp minus the capture time on the air. The three
clocks share no offset, so each figure is reported above its fastest frame;
oscillator drift over 60 s is a few milliseconds at most.
"""
import argparse
import collections
import json
from pathlib import Path

SENDER_MAC = 'E0:72:A1:D5:64:0C'


def air_times(sniffer):
    result = {}
    for line in sniffer.splitlines():
        p = line.split()
        if (len(p) >= 7 and p[1] == 'CAP' and p[3] == SENDER_MAC and
                p[6][2:4] == '02'):
            counter = int.from_bytes(bytes.fromhex(p[6][8:16]), 'little')
            result.setdefault(counter, int(p[2]))  # First copy on the air.
    return result


def sends(sender):
    """Sequence -> (frame counter, capture-to-send microseconds)."""
    result = {}
    counter = None
    for line in sender.splitlines():
        p = line.split()
        if len(p) > 6 and p[1] == 'TX_DATA' and p[2] == '2':
            counter = int(p[5])
        elif len(p) > 8 and p[1] == 'OPUS_TX':
            result[int(p[2])] = (counter, int(p[7]))
    return result


def arrivals(receiver):
    """Sequence -> (arrival, capture time) for frames stored once."""
    result = {}
    for line in receiver.splitlines():
        p = line.split()
        if len(p) >= 7 and p[1] == 'ARRIVAL' and p[6] == '0':
            result[int(p[2])] = (int(p[4]), int(p[5]))
    return result


def summary(delays):
    low = min(delays)
    v = sorted((d - low) / 1000 for d in delays)
    pick = lambda q: round(v[min(len(v) - 1, int(q * len(v)))], 1)
    bins = collections.Counter(min(int(d // 5) * 5, 60) for d in v)
    return {'n': len(v), 'median_ms': pick(.5), 'p90_ms': pick(.9),
            'p99_ms': pick(.99), 'max_ms': round(v[-1], 1),
            'histogram_5ms': {f'{k}+' if k == 60 else str(k): bins[k]
                              for k in sorted(bins)}}


def analyse(sender, receiver, sniffer):
    air = air_times(sniffer)
    sent = sends(sender)
    send_air, air_loop = [], []
    for sequence, (arrival, capture) in arrivals(receiver).items():
        counter, to_send = sent.get(sequence, (None, 0))
        if counter in air:
            send_air.append(air[counter] - (capture + to_send))
            air_loop.append(arrival - air[counter])
    return {'send_to_air': summary(send_air),
            'air_to_loop': summary(air_loop)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', help='Log prefix, e.g. evidence/stage6/plain')
    args = parser.parse_args()
    logs = [Path(f'{args.run}_{name}.txt').read_text(errors='replace')
            for name in ('sender', 'receiver', 'sniffer')]
    print(json.dumps(analyse(*logs), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
