#!/usr/bin/env python3
"""Stage 6: on-air frames, Opus packet integrity, latency and audio; no keys."""
import argparse
import collections
import json
import math
import re
import zlib
from pathlib import Path

MARKERS = [b'HELLO SE19', b'SE19OPUS', b'SE19ECHO', b'SE19FILL']
OUTCOMES = ('decoded', 'concealed', 'decode_failed', 'failed')


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


def sent_packets(sender):
    """Opus packets the sender logged, by sequence: (crc, bytes)."""
    result = {}
    for m in re.finditer(r'OPUS_TX (\d+) (\d+) ([0-9A-F]{8}) \d+ \d+ -?\d+ 1 ([0-9A-Fa-f]+)', sender):
        packet = bytes.fromhex(m[4])
        if len(packet) != int(m[2]) or zlib.crc32(packet) != int(m[3], 16):
            raise ValueError('OPUS_TX line does not match its length or CRC')
        result[int(m[1])] = (int(m[3], 16), packet)
    return result


def packet_hits(frame, packets, lengths):
    """Sent Opus packets appearing anywhere in a frame's payload region."""
    region = frame[8:-32]
    return {region[i:i+n] for n in lengths for i in range(len(region) - n + 1)} & packets


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
            key = (f[2], f[3], len(f), f[8:16])
            regions[key].add(int.from_bytes(f[4:8], 'little'))
    collisions = sum(len(counters) - 1 for counters in regions.values())
    # A counter reused under one key shows as one header with two frames.
    # Only this boot's logged frames share keys; frames left on the air by
    # an earlier boot were sealed under older session keys.
    by_header = collections.defaultdict(set)
    for f in unique_sent:
        by_header[f[:8]].add(f)
    completeness = len(matched) / len(unique_sent) if unique_sent else 0
    # Real Opus packets on the air, not only the fixed marker.
    packets = {p for _, p in sent_packets(sender).values()}
    lengths = {len(p) for p in packets}
    voice = [f for f in selected if f[1] == 2 and f[2] == 1 and f[3] == 2]
    found = [packet_hits(f, packets, lengths) for f in voice]
    with_packet = sum(bool(h) for h in found)
    seen = set().union(*found) if found else set()
    opus_ok = (with_packet == len(voice) > 0 and len(seen) == len(packets)
               if plain else not seen) and bool(packets)
    return {
        'queued_unique': len(unique_sent), 'captured_unique': len(unique_capture),
        'matched_unique': len(matched), 'completeness': completeness,
        'marker_hits': hits, 'marker_frame_fraction': marked / len(selected) if selected else 0,
        'retransmissions': len(selected) - len(set(selected)),
        'unlogged_frames': len(unique_capture - unique_sent),
        'same_header_different_frames': sum(len(v) > 1 for v in by_header.values()),
        'repeated_marker_ciphertext_across_counters': collisions,
        'clear_headers': sorted({(f[1], f[2], f[3]) for f in selected}),
        'clear_counter_range': ([min(int.from_bytes(f[4:8], 'little') for f in selected),
                                 max(int.from_bytes(f[4:8], 'little') for f in selected)]
                                if selected else []),
        'opus_packets_sent': len(packets), 'opus_voice_frames_captured': len(voice),
        'opus_frames_with_sent_packet': with_packet,
        'opus_distinct_packets_on_air': len(seen),
        'pass': bool(selected) and completeness >= .95 and opus_ok and
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


def integrity(sender, receiver):
    """O1: every sent packet received once, with the same CRC."""
    sent = {seq: crc for seq, (crc, _) in sent_packets(sender).items()}
    received = {}
    for m in re.finditer(r'OPUS_RX (\d+) (\d+) ([0-9A-F]{8})', receiver):
        received.setdefault(int(m[1]), int(m[3], 16))
    matching = sum(received.get(seq) == crc for seq, crc in sent.items())
    return dict(sent=len(sent), received=len(received), matching_crc=matching,
                mismatched_crc=sum(seq in received and received[seq] != crc
                                   for seq, crc in sent.items()),
                missing=len(set(sent) - set(received)),
                **{"pass": len(sent) == 3000 and matching == 3000})


def opus_timing(sender, receiver, rtt_us, depth=60000):
    """O3/O5: per-step times and the mouth-to-ear estimate."""
    tx = [tuple(map(int, m.groups())) for m in
          re.finditer(r'OPUS_TX \d+ \d+ [0-9A-F]{8} (\d+) (\d+) (-?\d+) 1 ', sender)]
    play = [tuple(map(int, m.groups())) for m in
            re.finditer(r'OPUS_PLAY \d+ (\d) (\d+) (-?\d+)', receiver)]
    generate, encode, to_send = (summary([t[i] for t in tx]) for i in range(3))
    decode = summary([p[1] for p in play if p[0] == 0])
    conceal = summary([p[1] for p in play if p[0] != 0])
    lag = summary([p[2] for p in play])
    outcomes = collections.Counter(OUTCOMES[p[0]] for p in play)

    def estimate(key):
        parts = [20000, to_send.get(key, 0), rtt_us.get(key, 0) / 2, depth,
                 lag.get(key, 0), decode.get(key, 0)]
        return sum(parts)
    worst_step = max([v.get('max', 0) for v in (generate, encode, decode, conceal)])
    return dict(generate_us=generate, encode_us=encode, capture_to_send_us=to_send,
                decode_us=decode, conceal_us=conceal, play_lag_us=lag,
                live_outcomes=dict(outcomes), frames_played=len(play),
                worst_codec_step_us=worst_step,
                mouth_to_ear_us=dict(median=estimate('median'), p99=estimate('p99')),
                mouth_to_ear_terms='20 ms frame + capture-to-send + RTT/2 + '
                                   'depth + play lag + decode')


def quality(receiver):
    """Firmware audio comparisons: live, in-order and replayed depths."""
    result = {}
    pattern = (r'QUALITY (\w+) (\d+) decoded (\d+) concealed (\d+) decode_failed (\d+) '
               r'failed (\d+) corr (\S+) snr (\S+) worst (\S+) windows (\d+)')
    for m in re.finditer(pattern, receiver):
        name = m[1] if m[1] != 'replay' else f'replay_{m[2]}'
        result[name] = dict(depth=int(m[2]), decoded=int(m[3]), concealed=int(m[4]),
                            decode_failed=int(m[5]), failed=int(m[6]),
                            correlation=float(m[7]), snr_db=float(m[8]),
                            worst_window_snr_db=float(m[9]), windows=int(m[10]))
    missed = re.findall(r'LIVE_MISSED (\d+)', receiver)
    if missed:
        result['live_missed'] = int(missed[-1])
    return result


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
        result['integrity'] = integrity(sender, receiver)
        result['opus_timing'] = opus_timing(sender, receiver,
                                            result['latency']['rtt_us'])
        result['quality'] = quality(receiver)
    if args.baseline and 'latency' in result:
        baseline = json.loads(args.baseline.read_text())['latency']
        result['added_one_way_median_us'] = (result['latency']['estimated_one_way_us']['median'] -
                                           baseline['estimated_one_way_us']['median'])
    print(json.dumps(result, indent=2))
    return 0 if result['capture']['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
