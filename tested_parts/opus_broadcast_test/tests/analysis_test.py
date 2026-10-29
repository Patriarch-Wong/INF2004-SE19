import importlib.util
from pathlib import Path
import unittest
import zlib

spec = importlib.util.spec_from_file_location(
    'analysis', Path(__file__).resolve().parents[1] / 'tools/analyse_broadcast.py')
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)

FRAMES = a.FRAMES


def packet(seq):
    return bytes((seq * 31 + i * 7) % 256 for i in range(60))


def crc(seq):
    return f'{zlib.crc32(packet(seq)):08X}'


def boot(board):
    return [f'ESP-NOW Opus broadcast | board {board} | channel 11 | '
            'power save off']


def sender_log(board, stream, mode, heard=0, send_errors=(),
               failed_callbacks=()):
    lines = boot(board)
    lines.append(f'STREAM_TX_START {stream} mode {mode} frames {FRAMES} '
                 f'board {board} 1')
    for s in range(FRAMES):
        result = 1 if s in send_errors else 0
        previous = int(mode == 'red' and s > 0)
        lines.append(f'OPUS_TX {s} 60 {crc(s)} 10 6000 7000 {result} '
                     f'{155 if previous else 93} {previous}')
        if not result:
            lines.append(f'SEND_CB {s} {0 if s in failed_callbacks else 1}')
    sent = FRAMES - len(send_errors)
    lines.append(f'TX_REPORT {stream} mode {mode} frames {FRAMES} sent '
                 f'{sent} send_errors {len(send_errors)} build_errors 0 '
                 f'callbacks_ok {sent} callbacks_failed 0 callback_drops 0 '
                 f'unmatched 0 heard {heard}')
    lines.append('VOICE_DROPS timer 0 radio 0 encode 0 codec 0 play_log 0')
    return lines


def quality(name, depth, concealed, corr=0.9989, snr=26.73):
    return (f'QUALITY {name} {depth} decoded {FRAMES - concealed} concealed '
            f'{concealed} decode_failed 0 failed 0 corr {corr:.4f} snr '
            f'{snr:.2f} worst 20.00 windows 60')


def receiver_log(board, stream, sender, mode, lost=(), delay=None,
                 bad_crc=(), uncopied=()):
    """A receiver's log: frame s arrives at s * 20 ms + delay[s]."""
    delay = delay or {}
    lines = boot(board)
    lines.append(f'STREAM_LOCK {stream} sender {sender} frames {FRAMES} '
                 'anchor 0')
    primary, earliest = {}, {}
    for s in sorted(set(range(FRAMES)) - set(lost),
                    key=lambda s: s * 20000 + delay.get(s, 0)):
        at = s * 20000 + delay.get(s, 0)
        code = '00000000' if s in bad_crc else crc(s)
        first = s not in earliest
        lines.append(f'COPY {s} P 60 {code} {at} {0 if first else 1}')
        primary[s] = at
        earliest.setdefault(s, at)
        if mode == 'red' and s > 0 and s - 1 not in uncopied:
            first = s - 1 not in earliest
            lines.append(f'COPY {s - 1} R 60 {crc(s - 1)} {at} '
                         f'{0 if first else 1}')
            earliest.setdefault(s - 1, at)
    sources = {}
    for depth in a.DEPTHS_US:
        counts = {k: 0 for k in a.SOURCES}
        for s in range(FRAMES):
            counts[a.choose(primary.get(s), earliest.get(s), s, 0, depth)] += 1
        sources[depth] = counts
    live = sources[200000]
    for s in range(FRAMES):
        letter = a.LETTERS[a.choose(primary.get(s), earliest.get(s), s, 0,
                                    200000)]
        outcome = 1 if letter == 'C' else 0
        lines.append(f'OPUS_PLAY {s} {letter} {outcome} 2400 2500')
    lines.append(f'RX_REPORT {stream} sender {sender} anchor 0 frames '
                 f'{FRAMES - len(lost)} redundant_copies 0 redundant_first 0 '
                 'duplicates 0 mismatches 0 foreign 0 stale 0 wrong_frames 0 '
                 f'played {FRAMES} abandoned 0')

    def line(name, depth, c):
        return (f'SOURCES {name} {depth} primary {c["primary"]} redundant '
                f'{c["redundant"]} concealed {c["concealed"]}')
    lines.append(line('live', 200000, live))
    lines += [line('sweep', d, sources[d]) for d in a.DEPTHS_US]
    lines.append(quality('live', 200000, live['concealed']))
    lines.append('LIVE_MISSED 0')
    lines.append(quality('copies', a.IN_ORDER, FRAMES - len(earliest)))
    lines.append('VOICE_DROPS timer 0 radio 0 encode 0 codec 0 play_log 0')
    lines.append(f'RX_REPORT_END {stream}')
    return lines


def stamp(lines):
    """Capture timestamps, as capture_logs.py writes them."""
    return '\n'.join(f'{i / 50:6.2f} {text}' for i, text in enumerate(lines))


def capture(streams, receiver_options=None):
    """streams: (sender, id, mode) in order; the other two boards receive."""
    receiver_options = receiver_options or {}
    logs = {b: [] for b in (1, 2, 3)}
    for b in logs:
        logs[b] += boot(b)
    for sender, stream, mode in streams:
        for b in logs:
            if b == sender:
                logs[b] += sender_log(b, stream, mode)[1:]
            else:
                options = receiver_options.get(b, {})
                logs[b] += receiver_log(b, stream, sender, mode,
                                        **options)[1:]
    return {f'board{b}': stamp(lines) for b, lines in logs.items()}


ALL_SIX = [(1, 'A0000001', 'plain'), (1, 'A0000002', 'red'),
           (2, 'A0000003', 'red'), (2, 'A0000004', 'plain'),
           (3, 'A0000005', 'plain'), (3, 'A0000006', 'red')]


class AnalysisTest(unittest.TestCase):
    def test_bursts(self):
        self.assertEqual(a.bursts([]), [])
        self.assertEqual(a.bursts([4, 1, 2, 3, 9, 11, 12]), [4, 1, 2])

    def test_clean_capture_passes(self):
        result = a.analyse(capture(ALL_SIX))
        v = result['verdicts']
        self.assertEqual(result['boards'], [1, 2, 3])
        self.assertEqual(len(result['streams']), 6)
        for name in ('B1_any_board_sends', 'B2_integrity',
                     'B4_redundancy_recovery', 'B5_played_200ms',
                     'B6_codec_quality', 'B9_silent_receivers'):
            self.assertTrue(v[name]['pass'], name)
        self.assertTrue(v['firmware_cross_check'])
        stream = result['streams'][1]
        self.assertEqual(sorted(stream['receivers']), ['2', '3'])
        self.assertEqual(stream['sender']['with_previous'], FRAMES - 1)
        estimate = stream['delay_estimate']['2']
        self.assertEqual(estimate['total_median_us'],
                         20000 + 7000 + 12700 + 800 + 200000 + 2500 + 2400)

    def test_losses_shared_and_recovered(self):
        options = {2: dict(lost=[10, 11, 500]), 3: dict(lost=[500, 700])}
        result = a.analyse(capture([(1, 'B1', 'red')], options))
        s = result['streams'][0]
        r2, r3 = s['receivers']['2'], s['receivers']['3']
        self.assertEqual(r2['primary_lost'], 3)
        self.assertEqual(r2['loss_bursts']['longest'], 2)
        self.assertEqual(r2['loss_bursts']['lengths'], {2: 1, 1: 1})
        # 11 and 500 come back from the next frame; 10's carrier was lost.
        self.assertEqual(r2['recovered_by_copy'], 2)
        self.assertEqual(r2['no_copy_sequences'], [10])
        self.assertEqual(r2['unrecovered_with_next_frame'], 0)
        self.assertEqual(r2['sources_from_log'][200000]['concealed'], 1)
        self.assertEqual(r3['no_copy'], 0)
        self.assertEqual(s['shared_loss']['lost_at_both'], 1)
        self.assertEqual(s['shared_loss']['lost_at_one'], 3)
        self.assertEqual(s['shared_loss']['lost_at_one_by_board'],
                         {'2': 2, '3': 1})
        self.assertTrue(result['verdicts']['B4_redundancy_recovery']['pass'])
        self.assertTrue(result['verdicts']['firmware_cross_check'])

    def test_plain_losses_are_not_recovered(self):
        options = {2: dict(lost=[10]), 3: dict(lost=[])}
        result = a.analyse(capture([(1, 'C1', 'plain')], options))
        r2 = result['streams'][0]['receivers']['2']
        self.assertEqual(r2['recovered_by_copy'], 0)
        self.assertEqual(r2['no_copy'], 1)
        self.assertEqual(r2['live_sources']['concealed'], 1)
        self.assertAlmostEqual(r2['live_late_or_missing_fraction'],
                               1 / FRAMES)

    def test_too_many_unrecoverable_losses_fail_b4(self):
        lost = [s for s in range(0, 30, 2)] + [s + 1 for s in range(0, 30, 2)]
        result = a.analyse(capture([(1, 'D1', 'red')],
                                   {2: dict(lost=lost)}))
        r2 = result['streams'][0]['receivers']['2']
        self.assertEqual(r2['no_copy'], 29)
        self.assertFalse(result['verdicts']['B4_redundancy_recovery']['pass'])

    def test_loss_with_next_frame_must_be_recovered(self):
        # Frame 11 arrived without its copy of 10 (the sender skipped it).
        options = {2: dict(lost=[10], uncopied=[10])}
        result = a.analyse(capture([(1, 'D2', 'red')], options))
        r2 = result['streams'][0]['receivers']['2']
        self.assertEqual(r2['unrecovered_with_next_frame'], 1)
        self.assertLess(r2['no_copy_fraction'], .005)
        self.assertFalse(result['verdicts']['B4_redundancy_recovery']['pass'])

    def test_crc_mismatch_fails_b2(self):
        result = a.analyse(capture([(1, 'E1', 'red')],
                                   {3: dict(bad_crc=[42])}))
        self.assertEqual(result['streams'][0]['receivers']['3']
                         ['crc_mismatches'], 1)
        self.assertFalse(result['verdicts']['B2_integrity']['pass'])

    def test_late_frame_depends_on_depth(self):
        # Frame 100's own copy is 120 ms late; frame 101 carries it in time.
        options = {2: dict(delay={100: 120000})}
        result = a.analyse(capture([(1, 'F1', 'red')], options))
        sources = result['streams'][0]['receivers']['2']['sources_from_log']
        self.assertEqual(sources[60000]['redundant'], 1)
        self.assertEqual(sources[150000]['primary'], FRAMES)
        plain = a.analyse(capture([(1, 'F2', 'plain')], options))
        sources = plain['streams'][0]['receivers']['2']['sources_from_log']
        self.assertEqual(sources[60000]['concealed'], 1)
        self.assertEqual(sources[100000]['concealed'], 1)
        self.assertEqual(sources[150000]['concealed'], 0)

    def test_live_must_follow_the_logged_arrivals(self):
        logs = capture([(1, 'G1', 'red')], {2: dict(lost=[10])})
        result = a.analyse(logs)
        self.assertTrue(result['streams'][0]['receivers']['2']
                        ['live_matches_log'])
        # Slot 10 was recovered (R); a player that concealed it is wrong.
        logs['board2'] = logs['board2'].replace(
            'OPUS_PLAY 10 R 0', 'OPUS_PLAY 10 C 1', 1)
        result = a.analyse(logs)
        r2 = result['streams'][0]['receivers']['2']
        self.assertEqual(r2['live_slots_not_as_logged'], 1)
        self.assertFalse(r2['live_matches_log'])
        self.assertFalse(result['verdicts']['B5_played_200ms']['pass'])

    def test_heard_and_send_status(self):
        logs = capture([(1, 'H1', 'plain')], {2: dict(lost=[7, 8]),
                                               3: dict(lost=[7])})
        logs['board1'] = stamp(sender_log(1, 'H1', 'plain', heard=3,
                                          send_errors=[7],
                                          failed_callbacks=[8]))
        result = a.analyse(logs)
        shared = result['streams'][0]['shared_loss']
        self.assertEqual(shared['both_with_send_error'], 1)
        self.assertEqual(shared['one_with_failed_callback'], 1)
        self.assertFalse(result['verdicts']['B9_silent_receivers']['pass'])
        self.assertFalse(result['verdicts']['B1_any_board_sends']['pass'])

    def test_missing_receiver_or_mode_fails_b1(self):
        result = a.analyse(capture(ALL_SIX[:5]))
        self.assertFalse(result['verdicts']['B1_any_board_sends']['pass'])
        logs = capture(ALL_SIX)
        logs['board3'] = stamp(boot(3) + sender_log(3, 'A0000005', 'plain')[1:]
                               + sender_log(3, 'A0000006', 'red')[1:])
        result = a.analyse(logs)
        self.assertEqual(len(result['streams'][0]['receivers']), 1)
        self.assertFalse(result['verdicts']['B1_any_board_sends']['pass'])

    def test_slow_step_fails_b1(self):
        logs = capture([(1, 'J1', 'plain'), (1, 'J2', 'red'),
                        (2, 'J3', 'plain'), (2, 'J4', 'red'),
                        (3, 'J5', 'plain'), (3, 'J6', 'red')])
        logs['board2'] = logs['board2'].replace(
            'OPUS_PLAY 5 P 0 2400', 'OPUS_PLAY 5 P 0 20001', 1)
        result = a.analyse(logs)
        self.assertFalse(result['verdicts']['B1_any_board_sends']['pass'])
        self.assertEqual(result['verdicts']['B1_any_board_sends']['streams']
                         [0]['worst_step_us'], 20001)


if __name__ == '__main__':
    unittest.main()
