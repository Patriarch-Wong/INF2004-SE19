"""Capture the SE19 sniffer's serial stream to a Wireshark PCAP."""

import argparse
from collections import deque
import re
import signal
import struct
import time
from pathlib import Path


STATION_MACS = (
    "E0:72:A1:D8:E2:10", "E0:72:A1:D7:FA:B8", "E0:72:A1:D9:A4:94",
    "E0:72:A1:D8:0E:48", "E0:72:A1:D5:64:0C",
)
DEFAULT_MACS = STATION_MACS + tuple("E2" + mac[2:] for mac in STATION_MACS)
MAC_PATTERN = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\Z")
FRAME_PATTERN = re.compile(rb"FRAME (\d{1,20}) ((?:[0-9a-fA-F]{2}){24,4091})\Z")
PCAP_HEADER = struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 4091, 105)
BAUD = 921600


class PcapCapture:
    def __init__(self, output):
        self.output = output
        self.count = 0
        self.malformed = 0
        self.first_timestamp = None
        self.epoch_us = time.time_ns() // 1000
        output.write(PCAP_HEADER)

    def record(self, line):
        if not line.startswith(b"FRAME "):
            return
        fields = FRAME_PATTERN.fullmatch(line)
        if fields is None:
            self.malformed += 1
            return
        timestamp = int(fields[1])
        frame = bytes.fromhex(fields[2].decode("ascii"))
        if self.first_timestamp is None:
            self.first_timestamp = timestamp
        offset = timestamp - self.first_timestamp
        if not 0 <= offset <= 86_400_000_000:
            self.malformed += 1
            return
        seconds, micros = divmod(self.epoch_us + offset, 1_000_000)
        header = struct.pack("<IIII", seconds, micros, len(frame), len(frame))
        self.output.write(header + frame)
        self.count += 1


class SerialLines:
    """Keep partial lines across timeouts and a capture interrupted by Ctrl+C."""

    def __init__(self, port):
        self.port = port
        self.pending = b""
        self.discarding = False
        self.ready = deque()

    def read(self):
        if self.ready:
            return self.ready.popleft()
        chunk = self.port.read(min(self.port.in_waiting or 1, 8192))
        *complete, self.pending = (self.pending + chunk).split(b"\n")
        for line in complete:
            if not self.discarding:
                record = line.rstrip(b"\r") if len(line) <= 8220 else b"FRAME malformed-oversized-record"
                self.ready.append(record)
            self.discarding = False
        if self.discarding or len(self.pending) > 8220:
            if not self.discarding:
                self.ready.append(b"FRAME malformed-oversized-record")
            self.pending = b""
            self.discarding = True
        return self.ready.popleft() if self.ready else None


def start_capture(port, lines, channel, macs):
    # Stop a previous capture if its serial connection closed unexpectedly.
    port.write(b"\nstop\n")
    command = f"start {channel} {' '.join(macs)}\n".encode("ascii")
    for _ in range(3):
        port.write(command)
        until = time.monotonic() + 2
        while time.monotonic() < until:
            line = lines.read()
            if line == b"STARTED" or (line and line.startswith(b"STARTED ")):
                return
            if line and line.startswith(b"ERROR "):
                raise RuntimeError(line.decode("ascii", errors="replace"))
    raise RuntimeError("The sniffer did not respond. Check the COM port and press RESET on the spare board.")


def stop_capture(port, lines, capture):
    port.write(b"\nstop\n")
    until = time.monotonic() + 10
    while time.monotonic() < until:
        line = lines.read()
        if line is None:
            continue
        match = re.fullmatch(rb"STOPPED (\d+)", line)
        if match:
            return int(match[1])
        capture.record(line)
    return None


def run(args, serial):
    with serial.Serial(port=None, baudrate=BAUD, timeout=0.25, write_timeout=2) as port:
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        time.sleep(1)
        port.reset_input_buffer()
        lines = SerialLines(port)
        with args.output.open("xb") as output:
            capture = PcapCapture(output)
            started = False
            interrupted = False

            def request_stop(_signum, _frame):
                nonlocal interrupted
                interrupted = True

            try:
                start_capture(port, lines, args.channel, args.mac or DEFAULT_MACS)
                started = True
                print(f"Capturing channel {args.channel}. Restart the handsets now. Ctrl+C stops capture.", flush=True)
                until = time.monotonic() + args.seconds
                previous_handler = signal.signal(signal.SIGINT, request_stop)
                try:
                    while not interrupted and time.monotonic() < until:
                        line = lines.read()
                        if line is not None:
                            capture.record(line)
                finally:
                    signal.signal(signal.SIGINT, previous_handler)
            except KeyboardInterrupt:
                pass
            finally:
                drops = stop_capture(port, lines, capture) if started else None
    drop_summary = "unknown (no stop confirmation)" if drops is None else str(drops)
    print(f"Saved {capture.count} frames to {args.output}. Queue drops: {drop_summary}. Malformed: {capture.malformed}.")
    if not capture.count:
        raise RuntimeError("No frames saved. Check the channel, board addresses and distance to the mesh.")
    return 0 if drops == 0 and capture.malformed == 0 else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="Sniffer UART port, such as COM7")
    parser.add_argument("output", type=Path, help="New .pcap filename")
    parser.add_argument("--channel", type=int, choices=range(1, 14), default=11)
    parser.add_argument("--seconds", type=int, choices=range(1, 601), default=180, metavar="1-600")
    parser.add_argument("--mac", action="append", help="Allowed station or SoftAP MAC, repeat to override stock boards")
    args = parser.parse_args()
    if args.output.suffix.lower() != ".pcap":
        parser.error("Choose an output filename ending in .pcap")
    if args.mac and (len(args.mac) > 20 or any(not MAC_PATTERN.fullmatch(mac) for mac in args.mac)):
        parser.error("Supply up to twenty MAC addresses in AA:BB:CC:DD:EE:FF format")
    try:
        import serial
    except ImportError:
        parser.exit(1, "Install pyserial with python -m pip install pyserial, or use PlatformIO's Python.\n")
    try:
        return run(args, serial)
    except (OSError, RuntimeError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
