#!/usr/bin/env python3
"""Compare exact on-air frames with local queued sends; no keys needed."""
import argparse
import collections
import json
import math
import re
from pathlib import Path

MARKERS = [b'HELLO SE19', b'SE19VOICE', b'SE19ECHO', b'SE19FILL']


def frames(text, capture):
    pattern = (r'CAP \d+ (\S+) (\S+) (\d+) ([0-9A-Fa-f]+)' if capture else
               r'TX_DATA (\d+) (\d+) (\d+) (\d+) (\d+) 0 ([0-9A-Fa-f ]+)')
    result = []
    for match in re.finditer(pattern, text):
        raw = bytes.fromhex(match.groups()[-1])
        if capture and len(raw) != int(match[3]):
            raise ValueError('capture length mismatch')
        if len(raw) >= 40 and raw[0] == 1 and raw[1] in (1, 2):
            result.append(raw)
    return result


def analyse(sender, sniffer, plain=False):
    sent = frames(sender, False)
    captured = frames(sniffer, True)
    unique_sent, unique_capture = set(sent), set(captured)
    matched = unique_sent & unique_capture
    # Search every captured data payload, including reverse-direction echoes.
    selected = captured
    hits = {m.decode(): sum(m in f[8:-32] for f in selected) for m in MARKERS}
    marked = sum(any(m in f[8:-32] for m in MARKERS) for f in selected)
    # All voice messages have a fixed marker and filler. Compare equal
    # plaintext regions, not the varying sequence and timestamp fields.
    regions = collections.defaultdict(set)
    collisions = 0
    for f in unique_capture:
        if f[1] == 2 and len(f) >= 80:
            key = (f[2], f[3], len(f), f[8:17] if f[2] == 1 else f[8:16])
            regions[key].add(int.from_bytes(f[4:8], 'little'))
    collisions = sum(len(counters) - 1 for counters in regions.values())
    by_header = collections.defaultdict(set)
    for f in selected:
        by_header[f[:8]].add(f)
    completeness = len(matched) / len(unique_sent) if unique_sent else 0
    return {
        'queued_unique': len(unique_sent), 'captured_unique': len(unique_capture),
        'matched_unique': len(matched), 'completeness': completeness,
        'marker_hits': hits, 'marker_frame_fraction': marked / len(selected) if selected else 0,
        'retransmissions': len(selected) - len(set(selected)),
        'same_header_different_frames': sum(len(v) > 1 for v in by_header.values()),
        'repeated_marker_ciphertext_across_counters': collisions,
        'clear_headers': sorted({(f[1], f[2], f[3]) for f in selected}),
        'clear_counter_range': ([min(int.from_bytes(f[4:8], 'little') for f in selected),
                                 max(int.from_bytes(f[4:8], 'little') for f in selected)]
                                if selected else []),
        'pass': bool(selected) and completeness >= .95 and
                (marked == len(selected) if plain else not marked and not collisions) and
                not any(len(v) > 1 for v in by_header.values()),
    }


def summary(values):
    values = sorted(values)
    if not values:
        return {'n': 0}
    return dict(n=len(values), min=values[0], median=values[(len(values)-1)//2],
                p99=values[math.ceil(len(values)*.99)-1], max=values[-1])


def latency(sender, receiver):
    seal = [int(m[1]) for m in re.finditer(r'TX_DATA 2 1 2 \d+ (\d+) 0 ', sender)]
    opened = [int(m[1]) for m in re.finditer(r'RX_DATA 2 1 \d+ (\d+)', receiver)]
    rtt = [int(m[1]) for m in re.finditer(r'RTT \d+ (\d+)', sender)]
    arrivals = {}
    total = 3000  # Fixed firmware experiment, including total receive outage.
    anchor = None
    previous_transit = None
    jitter = 0.0
    for m in re.finditer(r'ARRIVAL (\d+) (\d+) (\d+) (\d+) 0', receiver):
        seq, frames_count, arrival, capture = map(int, m.groups())
        if seq in arrivals or frames_count != total:
            raise ValueError('duplicate stream or unexpected frame count; use one boot per log')
        arrivals[seq] = arrival
        if anchor is None:
            anchor = seq, arrival
        transit = arrival - capture
        if previous_transit is not None:
            jitter += (abs(transit - previous_transit) - jitter) / 16
        previous_transit = transit
    sweep = {}
    for depth in (20000, 40000, 60000, 80000):
        late = sum(t > anchor[1] + depth + (s-anchor[0])*20000
                   for s, t in arrivals.items()) if anchor else 0
        sweep[depth] = dict(on_time=len(arrivals)-late, late=late,
                            lost=total-len(arrivals),
                            late_lost_fraction=(late+total-len(arrivals))/total)
    seal_stats, open_stats = summary(seal), summary(opened)
    return dict(seal_us=seal_stats, open_us=open_stats, rtt_us=summary(rtt),
                estimated_one_way_us=summary([v/2 for v in rtt]),
                seal_interval_percent=summary([v/200 for v in seal]),
                open_interval_percent=summary([v/200 for v in opened]),
                jitter_us=jitter, playout=sweep,
                crypto_max_sum_us=max(seal, default=0)+max(opened, default=0),
                drops=re.findall(r'VOICE_DROPS[^\n]*', sender+'\n'+receiver))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sender', type=Path, required=True)
    parser.add_argument('--sniffer', type=Path, required=True)
    parser.add_argument('--receiver', type=Path)
    parser.add_argument('--plain', action='store_true')
    parser.add_argument('--baseline', type=Path, help='Plain analysis JSON for V1 minus V2')
    args = parser.parse_args()
    sender = args.sender.read_text()
    receiver = args.receiver.read_text() if args.receiver else ''
    result = {'capture': analyse(sender + '\n' + receiver,
                                 args.sniffer.read_text(), args.plain)}
    if args.receiver:
        result['latency'] = latency(sender, receiver)
    if args.baseline and 'latency' in result:
        baseline = json.loads(args.baseline.read_text())['latency']
        result['added_one_way_median_us'] = (result['latency']['estimated_one_way_us']['median'] -
                                           baseline['estimated_one_way_us']['median'])
    print(json.dumps(result, indent=2))
    return 0 if result['capture']['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
