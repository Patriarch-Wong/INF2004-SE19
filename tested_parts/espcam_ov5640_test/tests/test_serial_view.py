"""Host tests for the serial frame protocol (tools/serial_view.py).

    python3 -m unittest discover -s tests
"""

import pathlib
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent /
                       "tools"))

from serial_view import HEADER, MAGIC, FrameParser  # noqa: E402


def fake_jpeg(width, height):
    sof = b"\xff\xc0" + struct.pack(">HBHHB", 11, 8, height, width, 1)
    sof += b"\x01\x11\x00"
    return (b"\xff\xd8" + sof + b"\xff\xda\x00\x02" + bytes(range(40)) +
            b"\xff\xd9")


def wire_frame(jpeg, width, height, crc=None):
    crc = zlib.crc32(jpeg) if crc is None else crc
    return HEADER.pack(MAGIC, len(jpeg), width, height, crc) + jpeg


def run(parser, stream, chunk):
    events = []
    for i in range(0, len(stream), chunk):
        events += parser.feed(stream[i:i + chunk])
    return events


class FrameParserTest(unittest.TestCase):
    def test_text_and_frames_any_chunking(self):
        jpeg = fake_jpeg(640, 480)
        stream = (b"Self-test PASS\n" + wire_frame(jpeg, 640, 480) +
                  b"Frame size VGA: ok\n" + wire_frame(jpeg, 640, 480))
        for chunk in (1, 3, 7, 64, len(stream)):
            events = run(FrameParser(), stream, chunk)
            text = "".join(v for k, v in events if k == "text")
            frames = [v for k, v in events if k == "frame"]
            self.assertEqual(text, "Self-test PASS\nFrame size VGA: ok\n")
            self.assertEqual(len(frames), 2, chunk)
            self.assertTrue(all(f["ok"] and f["jpeg"] == jpeg
                                for f in frames))

    def test_bad_crc_and_wrong_size_flagged(self):
        jpeg = fake_jpeg(320, 240)
        stream = (wire_frame(jpeg, 320, 240, crc=1) +
                  wire_frame(jpeg, 640, 480) + wire_frame(jpeg, 320, 240))
        frames = [v for k, v in FrameParser().feed(stream) if k == "frame"]
        self.assertEqual([f["ok"] for f in frames], [False, False, True])

    def test_bogus_header_resyncs(self):
        jpeg = fake_jpeg(320, 240)
        bogus = MAGIC + struct.pack("<IHHI", 0xFFFFFFFF, 0, 0, 0)
        frames = [v for k, v in
                  FrameParser().feed(bogus + wire_frame(jpeg, 320, 240))
                  if k == "frame"]
        self.assertEqual(len(frames), 1)
        self.assertTrue(frames[0]["ok"])


if __name__ == "__main__":
    unittest.main()
