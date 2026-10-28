import importlib.util
from pathlib import Path
import os
import unittest
import zlib

spec = importlib.util.spec_from_file_location('analysis', Path(__file__).resolve().parents[1] / 'tools/analyse_capture.py')
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)
spec = importlib.util.spec_from_file_location('air', Path(__file__).resolve().parents[1] / 'tools/air_delay.py')
air = importlib.util.module_from_spec(spec)
spec.loader.exec_module(air)


def packet(seq):
    return bytes((seq * 31 + i * 7) % 256 for i in range(60))


def opus_body(seq):
    return (b'SE19OPUS' + seq.to_bytes(4, 'little') + (3000).to_bytes(4, 'little') +
            bytes(8) + (60).to_bytes(2, 'little') + packet(seq))


def frame(counter, body):
    return bytes([1, 2, 1, 2]) + counter.to_bytes(4, 'little') + body + bytes(32)


def logs(bodies, sequences=None):
    sequences = range(len(bodies)) if sequences is None else sequences
    items = [frame(i + 1, b) for i, b in enumerate(bodies)]
    tx = '\n'.join(f'TX_DATA 2 1 2 {i+1} 70 0 {f.hex()}' for i, f in enumerate(items))
    tx += '\n' + '\n'.join(
        f'OPUS_TX {s} 60 {zlib.crc32(packet(s)):08X} 9 6500 7000 1 {packet(s).hex()}'
        for s in sequences)
    cap = '\n'.join(f'CAP 100 A B {len(f)} {f.hex()}' for f in items)
    return tx, cap


class AnalysisTest(unittest.TestCase):
    def test_plain(self):
        tx, cap = logs([opus_body(0), opus_body(1)])
        result = a.analyse(tx, cap, True)
        self.assertTrue(result['pass'])
        self.assertEqual(result['opus_frames_with_sent_packet'], 2)
        self.assertFalse(a.analyse(tx, cap)['pass'])

    def test_plain_needs_every_packet(self):
        damaged = bytearray(opus_body(1))
        damaged[30] ^= 1
        tx, cap = logs([opus_body(0), bytes(damaged)])
        self.assertFalse(a.analyse(tx, cap, True)['pass'])
        # Every packet seen, but one voice frame carries none of them.
        tx, cap = logs([opus_body(0), opus_body(1), b'SE19OPUS' + bytes(78)],
                       sequences=[0, 1])
        result = a.analyse(tx, cap, True)
        self.assertEqual(result['opus_distinct_packets_on_air'], 2)
        self.assertFalse(result['pass'])

    def test_encrypted_and_duplicates(self):
        tx, cap = logs([os.urandom(86), os.urandom(86)])
        self.assertTrue(a.analyse(tx, cap)['pass'])
        self.assertEqual(a.analyse(tx, cap+'\n'+cap)['retransmissions'], 2)
        self.assertFalse(a.analyse(tx, '')['pass'])
        self.assertFalse(a.analyse('', cap)['pass'])

    def test_packet_on_air_fails_encrypted(self):
        leak = os.urandom(10) + packet(1) + os.urandom(16)
        tx, cap = logs([os.urandom(86), leak])
        result = a.analyse(tx, cap)
        self.assertEqual(result['opus_distinct_packets_on_air'], 1)
        self.assertFalse(result['pass'])

    def test_repeated_ciphertext(self):
        tx, cap = logs([b'x'*86, b'x'*86])
        self.assertFalse(a.analyse(tx, cap)['pass'])

    def test_reused_counter(self):
        tx, cap = logs([os.urandom(86), os.urandom(86)])
        # A frame from an earlier boot with this header: older session key.
        stale = frame(1, os.urandom(86))
        cap += f'\nCAP 50 A B {len(stale)} {stale.hex()}'
        result = a.analyse(tx, cap)
        self.assertEqual(result['unlogged_frames'], 1)
        self.assertEqual(result['same_header_different_frames'], 0)
        self.assertTrue(result['pass'])
        # The same boot logging two frames under one counter fails.
        reused = tx + f'\nTX_DATA 2 1 2 1 70 0 {stale.hex()}'
        result = a.analyse(reused, cap)
        self.assertEqual(result['same_header_different_frames'], 1)
        self.assertFalse(result['pass'])

    def test_bad_sender_line(self):
        with self.assertRaises(ValueError):
            a.sent_packets(f'OPUS_TX 0 60 00000000 9 6500 7000 1 {packet(0).hex()}')

    def test_integrity(self):
        tx = '\n'.join(f'OPUS_TX {s} 60 {zlib.crc32(packet(s)):08X} 9 6500 7000 1 '
                       f'{packet(s).hex()}' for s in range(3000))
        rx = '\n'.join(f'OPUS_RX {s} 60 {zlib.crc32(packet(s)):08X}' for s in range(3000))
        self.assertTrue(a.integrity(tx, rx)['pass'])
        bad = rx.replace(f'OPUS_RX 7 60 {zlib.crc32(packet(7)):08X}', 'OPUS_RX 7 60 00000000')
        result = a.integrity(tx, bad)
        self.assertFalse(result['pass'])
        self.assertEqual(result['mismatched_crc'], 1)
        result = a.integrity(tx, '\n'.join(rx.split('\n')[1:]))
        self.assertEqual(result['missing'], 1)
        self.assertFalse(result['pass'])

    def test_timing_and_quality(self):
        tx = 'OPUS_TX 0 60 00000000 10 6000 7000 1 \nOPUS_TX 1 60 00000000 10 6000 9000 1 '
        rx = 'OPUS_PLAY 0 0 2400 2500\nOPUS_PLAY 1 1 800 2600'
        result = a.opus_timing(tx, rx, {'median': 30000, 'p99': 50000})
        self.assertEqual(result['decode_us']['median'], 2400)
        self.assertEqual(result['live_outcomes'], {'decoded': 1, 'concealed': 1})
        self.assertEqual(result['mouth_to_ear_us']['median'],
                         20000 + 7000 + 15000 + 60000 + 2500 + 2400)
        line = ('QUALITY live 60000 decoded 2990 concealed 10 decode_failed 0 failed 0 '
                'corr 0.9970 snr 21.50 worst 12.25 windows 60\nLIVE_MISSED 0\n'
                'QUALITY replay 20000 decoded 2800 concealed 200 decode_failed 0 '
                'failed 0 corr 0.9 snr 10.00 worst 5.00 windows 60')
        parsed = a.quality(line)
        self.assertEqual(parsed['live']['concealed'], 10)
        self.assertEqual(parsed['replay_20000']['snr_db'], 10.0)
        self.assertEqual(parsed['live_missed'], 0)

    def test_lateness_loss_and_percentiles(self):
        result = a.latency('', 'ARRIVAL 0 3000 1000 0 0\nARRIVAL 1 3000 100000 20000 0')
        self.assertEqual(result['playout'][60000]['late'], 1)
        self.assertEqual(result['playout'][60000]['lost'], 2998)
        self.assertEqual(a.summary([4, 1, 3, 2])['median'], 2)
        self.assertEqual(a.latency('', '')['playout'][60000]['lost'], 3000)

    def test_air_delay(self):
        # Frame 0 leaves at once; frame 1 waits 30 ms in the sender's radio.
        tx, cap, rx = [], [], []
        for s, wait in ((0, 0), (1, 30000)):
            f = frame(s + 1, opus_body(s))
            tx += [f'1.0 TX_DATA 2 1 2 {s+1} 70 0 {f.hex()}',
                   f'1.0 OPUS_TX {s} 60 00000000 9 6500 7000 1 ']
            on_air = 500000 + s * 20000 + 7000 + wait
            cap.append(f'1.0 CAP {on_air} {air.SENDER_MAC} B {len(f)} {f.hex()}')
            rx.append(f'1.0 ARRIVAL {s} 3000 {on_air + 900000} {s * 20000} 0')
        result = air.analyse('\n'.join(tx), '\n'.join(rx), '\n'.join(cap))
        self.assertEqual(result['send_to_air']['max_ms'], 30.0)
        self.assertEqual(result['air_to_loop']['max_ms'], 0.0)


if __name__ == '__main__':
    unittest.main()
