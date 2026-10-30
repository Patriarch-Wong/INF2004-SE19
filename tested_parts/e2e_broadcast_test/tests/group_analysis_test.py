import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import analyse_group
from analysis_test import capture


def fixture(rate=6, **options):
    logs = capture([(1, 'A0000001', 'plain'), (1, 'A0000002', 'red')], **options)
    for board in (1, 2, 3):
        logs[f'board{board}'] += '\nBuild mode: encrypted group broadcast (AES-GCM), no relay\nRADIO channel 11 rate ' + str(rate) + ' ok\n'
        if board != 1:
            logs[f'board{board}'] += 'SECURITY_RX 6000\n'
    return logs


class GroupAnalysisTest(unittest.TestCase):
    def test_clean(self):
        self.assertTrue(analyse_group.analyse(fixture())['pass'])

    def test_rate_1(self):
        self.assertTrue(analyse_group.analyse(fixture(rate=1), rate=1)['pass'])

    def test_wrong_rate(self):
        self.assertFalse(analyse_group.analyse(fixture(rate=6), rate=1)['pass'])
        self.assertFalse(analyse_group.analyse(fixture(rate=1))['pass'])

    def test_empty(self):
        self.assertFalse(analyse_group.analyse({})['pass'])

    def test_missing_receiver(self):
        logs = fixture()
        del logs['board3']
        self.assertFalse(analyse_group.analyse(logs)['pass'])

    def test_plaintext_firmware(self):
        logs = fixture()
        logs['board2'] = logs['board2'].replace('Build mode: encrypted group broadcast (AES-GCM), no relay', '')
        self.assertFalse(analyse_group.analyse(logs)['pass'])

    def test_wrong_key_reject(self):
        logs = fixture()
        logs['board3'] += 'SECURITY_REJECT total 1\n'
        self.assertFalse(analyse_group.analyse(logs)['pass'])

    def test_corruption(self):
        self.assertFalse(analyse_group.analyse(fixture(receiver_options={3: {'bad_crc': (15,)}}))['pass'])

    def test_loss(self):
        self.assertFalse(analyse_group.analyse(fixture(receiver_options={2: {'lost': tuple(range(100, 200))}}))['pass'])


if __name__ == '__main__':
    unittest.main()
