import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('analysis', Path(__file__).resolve().parents[1] / 'tools/analyse_capture.py')
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)


def frame(counter, body):
    return bytes([1, 2, 1, 2]) + counter.to_bytes(4, 'little') + body + bytes(32)


def logs(items):
    return ('\n'.join(f'TX_DATA 2 1 2 {i+1} 70 0 {f.hex()}' for i, f in enumerate(items)),
            '\n'.join(f'CAP 100 A B {len(f)} {f.hex()}' for f in items))


class AnalysisTest(unittest.TestCase):
    def test_plain(self):
        tx, cap = logs([frame(1, b'SE19VOICE'+b'x'*31)])
        self.assertTrue(a.analyse(tx, cap, True)['pass'])
        self.assertFalse(a.analyse(tx, cap)['pass'])

    def test_encrypted_and_duplicates(self):
        tx, cap = logs([frame(1, b'x'*40), frame(2, b'y'*40)])
        self.assertTrue(a.analyse(tx, cap)['pass'])
        self.assertEqual(a.analyse(tx, cap+'\n'+cap)['retransmissions'], 2)
        self.assertFalse(a.analyse(tx, '')['pass'])
        self.assertFalse(a.analyse('', cap)['pass'])

    def test_repeated_ciphertext(self):
        tx, cap = logs([frame(1, b'x'*40), frame(2, b'x'*40)])
        self.assertFalse(a.analyse(tx, cap)['pass'])

    def test_lateness_loss_and_percentiles(self):
        result = a.latency('', 'ARRIVAL 0 3000 1000 0 0\nARRIVAL 1 3000 100000 20000 0')
        self.assertEqual(result['playout'][60000]['late'], 1)
        self.assertEqual(result['playout'][60000]['lost'], 2998)
        self.assertEqual(a.summary([4, 1, 3, 2])['median'], 2)
        self.assertEqual(a.latency('', '')['playout'][60000]['lost'], 3000)


if __name__ == '__main__':
    unittest.main()
