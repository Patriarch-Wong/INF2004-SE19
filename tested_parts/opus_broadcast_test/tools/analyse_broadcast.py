#!/usr/bin/env python3
"""One-to-many Opus broadcast: one capture, every board's log, per stream.

Reads <run>_board1.txt, <run>_board2.txt and <run>_board3.txt (any that
exist), splits them by stream ID and reports, for each stream, integrity,
delivery and loss bursts per receiver, recovery by the redundant copy,
shared versus one-receiver losses, timing, the audio each receiver played
and the buffer sweep. The B1-B9 verdicts are at the end.

The firmware decodes each stream twice: live at 200 ms, and once in order
with every packet received. The sweep counts late and missing slots at
each depth from the logged arrivals; it has no audio figures.
"""
import argparse
import collections
import json
import math
import re
from pathlib import Path

FRAMES = 3000
INTERVAL_US = 20000
LIVE_DEPTH_US = 200000
DEPTHS_US = (60000, 100000, 150000, 200000, 300000)
IN_ORDER = 4294967295
# Copy results logged by the firmware (broadcast_receiver::copy_result).
STORED, LATER, DUPLICATE, MISMATCH = range(4)
SOURCES = ('primary', 'redundant', 'concealed')
LETTERS = {'primary': 'P', 'redundant': 'R', 'concealed': 'C'}
OUTCOMES = ('decoded', 'concealed', 'decode_failed', 'failed')
# Stage 6, encrypted channel 11 run (evidence/stage6/encrypted_air_delay.json):
# receivers send no echoes here, so there is no round trip to measure.
STAGE6_SEND_TO_AIR_MEDIAN_US = 12700
STAGE6_AIR_TO_LOOP_MEDIAN_US = 800
STEP_LIMIT_US = 20000

LINE = re.compile(r'^\s*\d+\.\d+ (.*)$')


def body(line):
    """The firmware text of a capture line, without its timestamp."""
    match = LINE.match(line)
    return match[1] if match else line


def summary(values):
    values = sorted(values)
    if not values:
        return {'n': 0}
    return dict(n=len(values), min=values[0],
                median=values[(len(values) - 1) // 2],
                p99=values[math.ceil(len(values) * .99) - 1], max=values[-1])


def quality_fields(parts):
    """QUALITY <name> <depth> decoded N concealed N ... windows N."""
    values = dict(zip(parts[3::2], parts[4::2]))
    return dict(decoded=int(values['decoded']),
                concealed=int(values['concealed']),
                decode_failed=int(values['decode_failed']),
                failed=int(values['failed']),
                correlation=float(values['corr']),
                snr_db=float(values['snr']),
                worst_window_snr_db=float(values['worst']),
                windows=int(values['windows']))


def new_sent(stream_id, board, mode):
    return dict(stream_id=stream_id, board=board, mode=mode, packets={},
                callbacks={}, report=None, drops=None)


def new_received(stream_id, board, sender, anchor):
    return dict(stream_id=stream_id, board=board, sender=sender,
                anchor=anchor, copies=[], plays=[], report=None,
                sources={}, quality={}, live_missed=None, drops=None,
                timeout=False)


def parse_log(text):
    """Board number, the streams it sent and the streams it received."""
    board = None
    sent, received = [], []
    tx = rx = None
    last = None  # The report a VOICE_DROPS line belongs to.
    for raw in text.splitlines():
        line = body(raw)
        p = line.split()
        if not p:
            continue
        if ' | board ' in line and line.startswith('ESP-NOW Opus broadcast'):
            board = int(re.search(r'\| board (\d+) \|', line)[1])
        elif p[0] == 'STREAM_TX_START' and len(p) >= 9:
            tx = new_sent(p[1], board, p[3])
            sent.append(tx)
        elif p[0] == 'OPUS_TX' and tx and len(p) >= 10:
            seq = int(p[1])
            tx['packets'][seq] = dict(
                size=int(p[2]), crc=p[3], generate_us=int(p[4]),
                encode_us=int(p[5]), to_send_us=int(p[6]),
                send_result=int(p[7]), frame_size=int(p[8]),
                previous=int(p[9]))
        elif p[0] == 'SEND_CB' and tx and len(p) >= 3:
            tx['callbacks'][int(p[1])] = int(p[2])
        elif p[0] == 'TX_REPORT' and tx and p[1] == tx['stream_id']:
            values = dict(zip(p[2::2], p[3::2]))
            tx['report'] = {k: v if k == 'mode' else int(v)
                            for k, v in values.items()}
            last = tx
        elif p[0] == 'STREAM_LOCK' and len(p) >= 8:
            rx = new_received(p[1], board, int(p[3]), int(p[7]))
            received.append(rx)
        elif p[0] == 'COPY' and rx and len(p) >= 7:
            rx['copies'].append(dict(sequence=int(p[1]), kind=p[2],
                                     size=int(p[3]), crc=p[4],
                                     arrival=int(p[5]), result=int(p[6])))
        elif p[0] == 'OPUS_PLAY' and rx and len(p) >= 6:
            rx['plays'].append(dict(sequence=int(p[1]), source=p[2],
                                    outcome=int(p[3]), decode_us=int(p[4]),
                                    lag_us=int(p[5])))
        elif p[0] == 'STREAM_TIMEOUT' and rx and p[1] == rx['stream_id']:
            rx['timeout'] = True
        elif p[0] == 'RX_REPORT' and rx and p[1] == rx['stream_id']:
            values = dict(zip(p[2::2], p[3::2]))
            rx['report'] = {k: int(v) for k, v in values.items()}
            last = rx
        elif p[0] == 'SOURCES' and rx and len(p) >= 9:
            name = 'live' if p[1] == 'live' else f'sweep_{p[2]}'
            rx['sources'][name] = {p[3]: int(p[4]), p[5]: int(p[6]),
                                   p[7]: int(p[8])}
        elif p[0] == 'QUALITY' and rx and len(p) > 3 and p[3] != 'error':
            depth = int(p[2])
            name = p[1] if p[1] == 'live' else (
                f'{p[1]}_in_order' if depth == IN_ORDER else
                f'{p[1]}_{depth}')
            rx['quality'][name] = dict(depth=depth, **quality_fields(p))
        elif p[0] == 'LIVE_MISSED' and rx:
            rx['live_missed'] = int(p[1])
        elif p[0] == 'VOICE_DROPS' and last is not None:
            values = dict(zip(p[1::2], p[2::2]))
            last['drops'] = {k: int(v) for k, v in values.items()}
            last = None
    return board, sent, received


def kept(copy):
    """A copy the receiver stored or matched, not a duplicate or mismatch."""
    return copy['result'] in (STORED, LATER)


def bursts(missing):
    """Lengths of runs of consecutive missing sequences."""
    lengths = []
    end = None
    for s in sorted(missing):
        if lengths and s == end + 1:
            lengths[-1] += 1
        else:
            lengths.append(1)
        end = s
    return lengths


def on_time(arrival, sequence, anchor, depth):
    return (arrival is not None and
            arrival <= depth + (sequence - anchor) * INTERVAL_US)


def choose(primary, earliest, sequence, anchor, depth):
    """broadcast_receiver::choose, from the logged arrivals."""
    if on_time(primary, sequence, anchor, depth):
        return 'primary'
    if on_time(earliest, sequence, anchor, depth):
        return 'redundant'
    return 'concealed'


def receiver_view(rx, sent_crc):
    """Delivery, integrity, recovery and slot choice for one receiver."""
    primary, earliest = {}, {}
    copies = collections.Counter()
    mismatched = []
    for c in rx['copies']:
        copies[c['kind']] += 1
        if c['crc'] != sent_crc.get(c['sequence']):
            mismatched.append(c['sequence'])
        if not kept(c):
            continue
        if c['kind'] == 'P':
            primary.setdefault(c['sequence'], c['arrival'])
        earliest[c['sequence']] = min(earliest.get(c['sequence'],
                                                   c['arrival']),
                                      c['arrival'])
    lost = sorted(set(range(FRAMES)) - set(primary))
    recovered = [s for s in lost if s in earliest]
    no_copy = [s for s in lost if s not in earliest]
    # A loss whose next frame arrived should come back from that frame.
    next_arrived = [s for s in lost if s + 1 in primary]
    unrecovered_next = [s for s in next_arrived if s not in earliest]
    anchor = rx['anchor']
    choices = {}
    for depth in DEPTHS_US:
        counts = collections.Counter(
            choose(primary.get(s), earliest.get(s), s, anchor, depth)
            for s in range(FRAMES))
        choices[depth] = {k: counts[k] for k in SOURCES}
    expected_live = {s: LETTERS[choose(primary.get(s), earliest.get(s), s,
                                       anchor, LIVE_DEPTH_US)]
                     for s in range(FRAMES)}
    lengths = bursts(lost)
    return dict(expected_live=expected_live,
        copies_logged=dict(copies), crc_mismatches=len(mismatched),
        primary_received=len(primary), primary_lost=len(lost),
        primary_loss_fraction=len(lost) / FRAMES,
        loss_bursts=dict(count=len(lengths), longest=max(lengths, default=0),
                         lengths=dict(collections.Counter(lengths))),
        recovered_by_copy=len(recovered), no_copy=len(no_copy),
        no_copy_fraction=len(no_copy) / FRAMES,
        losses_with_next_frame=len(next_arrived),
        unrecovered_with_next_frame=len(unrecovered_next),
        sources_from_log=choices, lost=lost, no_copy_sequences=no_copy)


def stream_view(tx, receivers):
    """Every figure for one stream: sender, each receiver, shared loss."""
    sent_crc = {s: p['crc'] for s, p in tx['packets'].items()}
    packets = tx['packets'].values()
    failed_send = {s for s, p in tx['packets'].items()
                   if p['send_result'] != 0}
    failed_callback = {s for s, ok in tx['callbacks'].items() if not ok}
    sender = dict(
        board=tx['board'], mode=tx['mode'], frames_logged=len(packets),
        send_errors=len(failed_send), callbacks=len(tx['callbacks']),
        callbacks_failed=len(failed_callback),
        with_previous=sum(p['previous'] for p in packets),
        generate_us=summary([p['generate_us'] for p in packets]),
        encode_us=summary([p['encode_us'] for p in packets]),
        capture_to_send_us=summary([p['to_send_us'] for p in packets]),
        report=tx['report'], drops=tx['drops'])
    views = {}
    for rx in receivers:
        view = receiver_view(rx, sent_crc)
        plays = rx['plays']
        decoded = [p['decode_us'] for p in plays if p['outcome'] == 0]
        concealed = [p['decode_us'] for p in plays if p['outcome'] != 0]
        lag = [p['lag_us'] for p in plays]
        live = rx['sources'].get('live', {})
        played = sum(live.values()) if live else 0
        # Each slot played what its logged arrivals say it should have.
        expected = view.pop('expected_live')
        wrong_slots = sum(p['source'] != expected.get(p['sequence'])
                          for p in plays)
        firmware_sources = {d: rx['sources'].get(f'sweep_{d}')
                            for d in DEPTHS_US}
        view.update(
            frames_played=len(plays), timeout=rx['timeout'],
            report=rx['report'], drops=rx['drops'],
            decode_us=summary(decoded), conceal_us=summary(concealed),
            play_lag_us=summary(lag),
            live_sources=live,
            live_late_or_missing_fraction=(live.get('concealed', 0) / played
                                           if played else None),
            live_missed=rx['live_missed'], live_slots_not_as_logged=wrong_slots,
            live_matches_log=bool(plays) and wrong_slots == 0,
            sources_match_firmware=all(
                firmware_sources[d] == view['sources_from_log'][d]
                for d in DEPTHS_US),
            quality=rx['quality'],
            sweep={d: dict(late_or_missing_fraction=
                           view['sources_from_log'][d]['concealed'] / FRAMES,
                           **view['sources_from_log'][d])
                   for d in DEPTHS_US})
        views[str(rx['board'])] = view
    shared = None
    if len(views) == 2:
        a, b = (set(v['lost']) for v in views.values())
        both, one = a & b, a ^ b
        shared = dict(
            lost_at_both=len(both), lost_at_one=len(one),
            lost_at_one_by_board={k: len(set(v['lost']) - (a & b))
                                  for k, v in views.items()},
            both_with_send_error=len(both & failed_send),
            both_with_failed_callback=len(both & failed_callback),
            one_with_send_error=len(one & failed_send),
            one_with_failed_callback=len(one & failed_callback))
    for view in views.values():
        del view['lost']
    return dict(stream_id=tx['stream_id'], sender=sender, receivers=views,
                shared_loss=shared)


def delay_estimate(stream):
    """20 ms frame + capture-to-send + stage 6 air + buffer + lag + decode."""
    to_send = stream['sender']['capture_to_send_us'].get('median', 0)
    result = {}
    for board, view in stream['receivers'].items():
        parts = dict(frame_us=INTERVAL_US, capture_to_send_us=to_send,
                     send_to_air_us=STAGE6_SEND_TO_AIR_MEDIAN_US,
                     air_to_loop_us=STAGE6_AIR_TO_LOOP_MEDIAN_US,
                     buffer_us=LIVE_DEPTH_US,
                     play_lag_us=view['play_lag_us'].get('median', 0),
                     decode_us=view['decode_us'].get('median', 0))
        result[board] = dict(total_median_us=sum(parts.values()), **parts)
    return result


def verdicts(streams, boards):
    """B1-B9 from the per-stream figures."""
    def red(s):
        return s['sender']['mode'] == 'red'

    complete = []
    for s in streams:
        report = s['sender']['report'] or {}
        drops = s['sender']['drops'] or {}
        steps = [s['sender']['generate_us'].get('max', 0),
                 s['sender']['encode_us'].get('max', 0)]
        ok = (s['sender']['frames_logged'] == FRAMES and
              report.get('sent') == FRAMES and
              drops.get('timer', 1) == 0 and drops.get('encode', 1) == 0 and
              s['sender']['send_errors'] == 0 and
              len(s['receivers']) == len(boards) - 1)
        for v in s['receivers'].values():
            steps += [v['decode_us'].get('max', 0),
                      v['conceal_us'].get('max', 0)]
            ok = ok and v['frames_played'] == FRAMES and not v['timeout']
        complete.append(dict(stream=s['stream_id'],
                             sender=s['sender']['board'],
                             mode=s['sender']['mode'], complete=ok,
                             worst_step_us=max(steps),
                             step_ok=max(steps) <= STEP_LIMIT_US))
    pairs = collections.defaultdict(set)
    for c in complete:
        if c['complete'] and c['step_ok']:
            pairs[c['sender']].add(c['mode'])
    receivers = [(s, v) for s in streams for v in s['receivers'].values()]
    b1 = (bool(boards) and
          all(pairs[b] == {'plain', 'red'} for b in boards))
    b2 = bool(receivers) and all(v['crc_mismatches'] == 0
                                 for _, v in receivers)
    red_rx = [(s, v) for s, v in receivers if red(s)]
    b4 = bool(red_rx) and all(
        v['unrecovered_with_next_frame'] == 0 and
        v['no_copy_fraction'] < .005 for _, v in red_rx)
    b5 = bool(red_rx) and all(
        v['live_late_or_missing_fraction'] is not None and
        v['live_late_or_missing_fraction'] < .01 and
        v['live_missed'] == 0 and v['live_matches_log']
        for _, v in red_rx)
    b6 = bool(red_rx) and all(
        (v['quality'].get('copies_in_order') or {}).get('correlation', 0) >
        .99 and
        (v['quality'].get('copies_in_order') or {}).get('snr_db', 0) > 20
        for _, v in red_rx)
    b9 = bool(streams) and all(
        (s['sender']['report'] or {}).get('heard', 1) == 0 for s in streams)
    return dict(
        B1_any_board_sends={'pass': b1, 'streams': complete},
        B2_integrity={'pass': b2},
        B3_raw_loss=dict(measurement=[
            dict(stream=s['stream_id'], sender=s['sender']['board'],
                 mode=s['sender']['mode'], receiver=k,
                 primary_lost=v['primary_lost'],
                 longest_burst=v['loss_bursts']['longest'])
            for s in streams for k, v in s['receivers'].items()]),
        B4_redundancy_recovery={'pass': b4},
        B5_played_200ms={'pass': b5, 'plain': [
            dict(stream=s['stream_id'], receiver=k,
                 late_or_missing=v['live_late_or_missing_fraction'])
            for s in streams if not red(s)
            for k, v in s['receivers'].items()]},
        B6_codec_quality={'pass': b6},
        B7_buffer_sweep='per stream: receivers.<board>.sweep',
        B8_loss_pattern=[dict(stream=s['stream_id'],
                              sender=s['sender']['board'],
                              mode=s['sender']['mode'], **s['shared_loss'])
                         for s in streams if s['shared_loss']],
        B9_silent_receivers={'pass': b9},
        firmware_cross_check=all(v['sources_match_firmware']
                                 for _, v in receivers))


def analyse(logs):
    """logs: {label: text}. Returns the full analysis."""
    sent, received, boards = [], [], []
    for text in logs.values():
        board, tx, rx = parse_log(text)
        if board is not None:
            boards.append(board)
        sent += tx
        received += rx
    streams = []
    for tx in sent:
        receivers = [r for r in received if r['stream_id'] == tx['stream_id']]
        stream = stream_view(tx, receivers)
        stream['delay_estimate'] = delay_estimate(stream)
        streams.append(stream)
    unmatched = sorted({r['stream_id'] for r in received} -
                       {t['stream_id'] for t in sent})
    return dict(boards=sorted(boards), streams=streams,
                streams_received_without_sender=unmatched,
                verdicts=verdicts(streams, sorted(boards)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', help='Log prefix, e.g. evidence/broadcast/run1')
    args = parser.parse_args()
    logs = {}
    for board in (1, 2, 3):
        path = Path(f'{args.run}_board{board}.txt')
        if path.exists():
            logs[f'board{board}'] = path.read_text(errors='replace')
    if not logs:
        parser.error(f'no {args.run}_boardN.txt logs')
    print(json.dumps(analyse(logs), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
