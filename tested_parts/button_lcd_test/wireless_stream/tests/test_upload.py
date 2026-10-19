"""Exercise the real uploader against a simulated serial protocol."""

from collections import deque
from contextlib import redirect_stdout
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools"))
import send_clip


class FakeSerial:
    def __init__(self, identity="FIRMWARE wireless_stream sender",
                 wrong_ack=False):
        self.identity = identity
        self.wrong_ack = wrong_ack
        self.replies = deque()
        self.commands = []
        self.data = bytearray()
        self.receiving = False

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def reset_input_buffer(self):
        self.replies.clear()

    def readline(self):
        return self.replies.popleft() if self.replies else b""

    def reply(self, line):
        self.replies.append((line + "\n").encode())

    def write(self, payload):
        if self.receiving:
            self.data.extend(payload)
            offset = len(self.data) + int(self.wrong_ack)
            self.reply(f"ACK {offset}")
            if len(self.data) == self.size:
                self.receiving = False
                self.reply(f"SAVED {self.size} {self.digest}")
        else:
            self.commands.append(payload)
            if payload == b"identity\n":
                self.reply(self.identity)
            elif payload == b"status\n":
                self.reply("FIRMWARE wireless_stream sender")
                self.reply("SOURCE /clip.lcdstream")
                self.reply("SD READY")
            elif payload.startswith(b"put "):
                _, size, self.digest = payload.decode().split()
                self.size = int(size)
                self.receiving = True
                self.reply("READY")
            else:
                raise AssertionError(f"Unexpected command: {payload}")
        return len(payload)


class UploadTest(unittest.TestCase):
    def run_upload(self, connection):
        serial_patch = patch.object(send_clip.serial, "Serial",
                                    return_value=connection)
        with serial_patch, \
                patch.object(send_clip.time, "sleep"), \
                patch.object(sys, "argv", ["send_clip.py", "--port", "fake"]), \
                redirect_stdout(io.StringIO()):
            send_clip.main()

    def test_current_sender_gets_the_small_clip(self):
        connection = FakeSerial()
        self.run_upload(connection)
        expected = (PROJECT.parent / "media/clip.lcdstream").read_bytes()
        self.assertEqual(connection.data, expected)
        self.assertEqual(len(connection.data), 1046349)
        self.assertEqual(connection.commands[0], b"identity\n")

    def test_old_firmware_rejected_before_put(self):
        connection = FakeSerial("ERROR unknown command")
        with self.assertRaisesRegex(RuntimeError, "Flash the"):
            self.run_upload(connection)
        self.assertEqual(connection.commands, [b"identity\n"])
        self.assertFalse(connection.data)

    def test_receiver_rejected_before_put(self):
        connection = FakeSerial("FIRMWARE wireless_stream receiver")
        with self.assertRaisesRegex(RuntimeError, "Wrong firmware/role"):
            self.run_upload(connection)
        self.assertEqual(connection.commands, [b"identity\n"])
        self.assertFalse(connection.data)

    def test_wrong_ack_stops_copy(self):
        connection = FakeSerial(wrong_ack=True)
        with self.assertRaisesRegex(RuntimeError, "Wrong acknowledgement"):
            self.run_upload(connection)
        self.assertEqual(len(connection.data), 4096)


if __name__ == "__main__":
    unittest.main()
