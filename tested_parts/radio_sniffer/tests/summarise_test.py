import io
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import summarise  # noqa: E402

# Lines in the exact format tests/sniffer_test.cpp checks.
LOG = """INFO radio_sniffer | keys: NONE | transmits: never
ESPNOW t=1000000 ch=11 rssi=-40 src=E0:72:A1:D8:E2:10/B1 dst=E0:72:A1:D7:FA:B8/B2 len=40 sm=1:voice:1>2:#1 data=AA
ESPNOW t=1020000 ch=11 rssi=-44 src=E0:72:A1:D8:E2:10/B1 dst=E0:72:A1:D7:FA:B8/B2 len=40 sm=1:voice:1>2:#2 data=BB
[12:00:01.000] ESPNOW t=1040000 ch=11 rssi=-42 src=E0:72:A1:D8:E2:10/B1 dst=E0:72:A1:D7:FA:B8/B2 len=40 sm=1:voice:1>2:#2 data=BB
ESPNOW t=1500000 ch=11 rssi=-60 src=02:11:22:33:44:55 dst=FF:FF:FF:FF:FF:FF/all len=12 sm=-
STATS espnow=on ch=11 captured=4
LORA t=2000000 mhz=915.000 sf=7 rssi=-50.5 snr=9.25 len=56 crc=ok sm=1:text:1>2:#7 data=01
LORA t=3000000 mhz=915.000 sf=7 rssi=-90.0 snr=-3.00 len=56 crc=bad sm=- data=02
ESPNOW t=broken ch=11 rssi=-1 len=1
"""


class SummariseTest(unittest.TestCase):
    def setUp(self):
        self.rows = summarise.summarise(LOG.splitlines(True))
        self.by = {(r["radio"], r["link"]): r for r in self.rows}

    def test_groups(self):
        self.assertEqual(len(self.rows), 4)

    def test_espnow_link(self):
        r = self.by[("ESPNOW", "E0:72:A1:D8:E2:10/B1 > "
                               "E0:72:A1:D7:FA:B8/B2")]
        self.assertEqual(r["frames"], 3)
        self.assertEqual(r["unique"], 2)
        self.assertEqual(r["types"], {"voice": 3})
        self.assertEqual((r["rssi_min"], r["rssi_median"], r["rssi_max"]),
                         (-44, -42, -40))
        self.assertEqual(r["seconds"], 0.04)
        self.assertEqual(r["per_second"], 50.0)

    def test_without_data(self):
        r = self.by[("ESPNOW", "02:11:22:33:44:55 > "
                               "FF:FF:FF:FF:FF:FF/all")]
        self.assertIsNone(r["unique"])
        self.assertIsNone(r["per_second"])
        self.assertEqual(r["types"], {"-": 1})

    def test_lora(self):
        ok = self.by[("LORA", "id 1>2")]
        self.assertEqual(ok["crc_bad"], 0)
        self.assertEqual(ok["types"], {"text": 1})
        bad = self.by[("LORA", "unknown")]
        self.assertEqual(bad["crc_bad"], 1)

    def test_broken_and_other_lines_skipped(self):
        self.assertIsNone(summarise.parse_line("ESPNOW t=x rssi=1 len=1"))
        self.assertIsNone(summarise.parse_line("STATS espnow=on"))

    def test_table(self):
        out = io.StringIO()
        summarise.print_table(self.rows, out)
        self.assertIn("3 frames (2 unique) over 0.04 s, 50.0/s", out.getvalue())
        out = io.StringIO()
        summarise.print_table([], out)
        self.assertIn("no ESPNOW or LORA lines", out.getvalue())


if __name__ == "__main__":
    unittest.main()
