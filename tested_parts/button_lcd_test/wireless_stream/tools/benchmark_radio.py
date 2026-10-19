"""Time verified two-board transfers; optionally sweep PHY rates/windows."""

import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import re
import time

import serial


class Board:
    def __init__(self, port, role, stack):
        self.role = role
        self.pending = bytearray()
        self.conn = serial.Serial(port=None, baudrate=921600, timeout=0,
                                  write_timeout=5)
        self.conn.dtr = False
        self.conn.rts = False
        self.conn.port = port
        self.conn.open()
        stack.enter_context(self.conn)

    def send(self, command):
        payload = (command + "\n").encode()
        if self.conn.write(payload) != len(payload):
            raise IOError("Incomplete command write")

    def lines(self):
        self.pending.extend(self.conn.read(self.conn.in_waiting))
        while b"\n" in self.pending:
            line, _, self.pending = self.pending.partition(b"\n")
            yield line.decode(errors="replace").strip()

    def expect(self, command, prefix, timeout=10):
        self.send(command)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in self.lines():
                if line.startswith("ERROR"):
                    raise RuntimeError(f"{self.role}: {line}")
                if line.startswith(prefix):
                    print(f"{self.role}: {line}", flush=True)
                    return line
            time.sleep(0.005)
        raise TimeoutError(f"{self.role}: no {prefix!r} after {command!r}")


def transfer(sender, receiver, timeout):
    started = None
    command_time = time.monotonic()
    deadline = command_time + timeout
    logs = []
    finished = {}
    sender.send("send")
    while time.monotonic() < deadline:
        for board in (sender, receiver):
            for line in board.lines():
                now = time.monotonic()
                logs.append({"seconds": round(now - command_time, 4),
                             "role": board.role, "line": line})
                print(f"{now - command_time:7.2f} {board.role}: {line}",
                      flush=True)
                if board is sender and line.startswith("SENDING "):
                    started = now
                match = re.search(
                    r"(?:STREAM SENT|RAM RECEIVED) VERIFIED (\d+) bytes "
                    r"CRC32 ([0-9a-fA-F]+)", line)
                if match:
                    finished[board.role] = (int(match[1]), match[2], now)
                if line.startswith("ERROR"):
                    raise RuntimeError(f"{board.role}: {line}")
        if len(finished) == 2:
            if not started or finished["sender"][:2] != \
                    finished["receiver"][:2]:
                raise RuntimeError("Missing start or mismatched verification")
            size, crc, end = finished["sender"]
            elapsed = end - started
            return {"bytes": size, "crc32": crc,
                    "seconds": elapsed, "kB_s": size / elapsed / 1000,
                    "command_seconds": end - command_time, "log": logs}
        time.sleep(0.002)
    raise TimeoutError("Transfer did not verify on both boards")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sender", required=True)
    parser.add_argument("--receiver", required=True)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--rates", type=int, nargs="+", default=[6, 12, 24, 54])
    parser.add_argument("--windows", type=int, nargs="+", default=[32])
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--playback", action="store_true")
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    results = []
    with ExitStack() as stack:
        sender = Board(args.sender, "sender", stack)
        receiver = Board(args.receiver, "receiver", stack)
        # Some USB-UART bridges reset the board on opening the port.
        time.sleep(3)
        for board in (sender, receiver):
            board.conn.reset_input_buffer()
            board.expect("identity", f"FIRMWARE wireless_stream {board.role}")
        autoplay_set = False
        try:
            if not args.baseline:
                receiver.expect(f"autoplay {int(args.playback)}", "AUTOPLAY ")
                autoplay_set = True
            for rate in ([None] if args.baseline else args.rates):
                if rate is not None:
                    for board in (sender, receiver):
                        board.expect(f"rate {rate}", f"RATE {rate} Mbps")
                for window in ([None] if args.baseline else args.windows):
                    if window is not None:
                        sender.expect(f"window {window}", f"WINDOW {window}")
                    for run in range(args.repeats):
                        result = transfer(sender, receiver, args.timeout)
                        result.update(rate_mbps=rate, window=window,
                                      run=run + 1, playback=args.playback or
                                      args.baseline)
                        results.append(result)
                        args.output.write_text(json.dumps(results, indent=2)
                                               + "\n")
                        print(f"RESULT rate={rate} window={window}: "
                              f"{result['kB_s']:.2f} kB/s, "
                              f"{result['seconds']:.3f} s", flush=True)
        finally:
            if autoplay_set:
                receiver.send("autoplay 1")


if __name__ == "__main__":
    main()
