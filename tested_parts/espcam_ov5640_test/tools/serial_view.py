#!/usr/bin/env python3
"""View the OV5640 over the USB cable: no Wi-Fi, no browser.

The board sends JPEG frames on its serial port; this script checks each one
(CRC-32 and JPEG size) and shows them in an ffplay window. Board text is
printed as it arrives. Uses the pyserial that ships with PlatformIO:

    PY=~/.platformio/penv/bin/python
    $PY tools/serial_view.py                     # live window, VGA
    $PY tools/serial_view.py --size 6            # live window, UXGA
    $PY tools/serial_view.py --snap --size 9     # save one 5 MP photo
    $PY tools/serial_view.py --save captures     # live + save every frame
    $PY tools/serial_view.py --port /dev/cu.usbmodem1101 --no-window

Press Ctrl-C to stop; the board's live mode is switched off on exit.
"""

import argparse
import pathlib
import shutil
import struct
import subprocess
import sys
import time
import zlib

from grab import SIZES, jpeg_size

MAGIC = b"OVJ1"
HEADER = struct.Struct("<4sIHHI")  # magic, length, width, height, crc32
MAX_FRAME_BYTES = 2 * 1024 * 1024
ASK_AGAIN_S = 3.0  # resend the start command after this long without frames


class FrameParser:
    """Splits the serial byte stream into text and JPEG frames."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        """Return a list of ("text", str) and ("frame", dict) events."""
        self.buf += data
        events = []
        while True:
            start = self.buf.find(MAGIC)
            if start < 0:
                # Keep a possible partial magic at the end.
                keep = len(MAGIC) - 1
                self._text(events, self.buf[:-keep] if keep else self.buf)
                del self.buf[:max(0, len(self.buf) - keep)]
                return events
            self._text(events, self.buf[:start])
            del self.buf[:start]
            if len(self.buf) < HEADER.size:
                return events
            _, length, width, height, crc = HEADER.unpack_from(self.buf)
            if length == 0 or length > MAX_FRAME_BYTES:
                del self.buf[:len(MAGIC)]  # not a real header
                continue
            end = HEADER.size + length
            if len(self.buf) < end:
                return events
            jpeg = bytes(self.buf[HEADER.size:end])
            del self.buf[:end]
            crc_ok = zlib.crc32(jpeg) == crc
            events.append(("frame", {
                "jpeg": jpeg,
                "width": width,
                "height": height,
                "ok": crc_ok and jpeg_size(jpeg) == (width, height),
            }))

    @staticmethod
    def _text(events, chunk):
        if chunk:
            events.append(("text", chunk.decode("utf-8", "replace")))


def find_port():
    from serial.tools import list_ports

    ports = [p.device for p in list_ports.comports()
             if "usbmodem" in p.device or "usbserial" in p.device
             or "wchusbserial" in p.device]
    if len(ports) == 1:
        return ports[0]
    names = ", ".join(ports) if ports else "none found"
    sys.exit(f"Pick the board with --port ({names})")


def open_port(port, baud):
    import serial

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    # Leave DTR/RTS low so opening the port does not reset the board.
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def start_window():
    if shutil.which("ffplay") is None:
        sys.exit("ffplay not found: brew install ffmpeg, or use --no-window")
    return subprocess.Popen(
        ["ffplay", "-hide_banner", "-loglevel", "error",
         "-window_title", "OV5640 (serial)",
         "-fflags", "nobuffer", "-flags", "low_delay", "-framedrop",
         "-sync", "ext", "-f", "mjpeg", "-framerate", "100", "-i", "-"],
        stdin=subprocess.PIPE)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", help="serial port (auto if only one)")
    parser.add_argument("--baud", type=int, default=2000000,
                        help="UART build baud; ignored by native USB")
    parser.add_argument("--size", type=int, default=1,
                        choices=range(len(SIZES)),
                        help="0 QVGA, 1 VGA ... 6 UXGA ... 9 QSXGA")
    parser.add_argument("--snap", action="store_true",
                        help="save one frame and exit")
    parser.add_argument("--save", metavar="DIR",
                        help="also save every live frame here")
    parser.add_argument("--no-window", action="store_true")
    args = parser.parse_args()

    ser = open_port(args.port or find_port(), args.baud)
    window = None if args.snap or args.no_window else start_window()
    save_dir = pathlib.Path(args.save or "captures")
    if args.snap or args.save:
        save_dir.mkdir(parents=True, exist_ok=True)

    # Commands sent while the board boots are lost (its serial port is not
    # open yet), so keep asking until frames arrive.
    start_cmd = str(args.size).encode() + (b"c" if args.snap else b"l")
    ser.write(b"x" + start_cmd)
    frames = bad = 0
    window_frames = window_bytes = 0
    window_start = last_heard = time.monotonic()
    fp = FrameParser()
    try:
        while True:
            data = ser.read(max(1, ser.in_waiting))
            for kind, value in fp.feed(data):
                if kind == "text":
                    sys.stderr.write(value)
                    sys.stderr.flush()
                    continue
                if not value["ok"]:
                    bad += 1
                    if args.snap:
                        ser.write(b"c")  # corrupted on the wire: retry
                    continue
                if window_frames == 0:
                    window_start = time.monotonic()
                frames += 1
                window_frames += 1
                last_heard = time.monotonic()
                window_bytes += len(value["jpeg"])
                if args.snap or args.save:
                    name = SIZES[args.size][0]
                    path = save_dir / (time.strftime("%Y%m%d_%H%M%S") +
                                       f"_{frames:05d}_{name}.jpg")
                    path.write_bytes(value["jpeg"])
                if args.snap:
                    print(f"Saved {value['width']}x{value['height']} "
                          f"{len(value['jpeg']) / 1024:.1f} KB to {path}")
                    return 0
                if window is not None:
                    try:
                        window.stdin.write(value["jpeg"])
                        window.stdin.flush()
                    except BrokenPipeError:
                        return 0  # window closed
            now = time.monotonic()
            if now - last_heard >= ASK_AGAIN_S:
                ser.write(start_cmd)
                last_heard = now
            if window_frames and now - window_start >= 2.0:
                elapsed = now - window_start
                print(f"[viewer] {window_frames / elapsed:.1f} fps | "
                      f"{window_bytes / elapsed / 1024:.0f} KB/s | "
                      f"{frames} frames, {bad} bad", file=sys.stderr)
                window_frames = window_bytes = 0
                window_start = now
    except KeyboardInterrupt:
        return 0
    except OSError as exc:  # SerialException is an OSError
        print(f"[viewer] serial port lost: {exc}", file=sys.stderr)
        return 1
    finally:
        try:
            ser.write(b"x")
            ser.close()
        except OSError:
            pass
        if window is not None:
            window.terminate()


if __name__ == "__main__":
    sys.exit(main())
