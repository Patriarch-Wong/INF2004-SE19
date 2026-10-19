#!/usr/bin/env python3
"""Trigger Opus + SD tests over UART, capture output, and fail on FAIL/timeout."""
import argparse
import sys
import time
from contextlib import ExitStack

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--output", help="Optional serial transcript path")
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")

    with ExitStack() as stack:
        log = stack.enter_context(open(args.output, "w")) if args.output else None
        port = serial.Serial(baudrate=115200, timeout=0.5)
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        stack.enter_context(port)
        # Let any boot/open transients settle, then explicitly request a run.
        time.sleep(1)
        port.reset_input_buffer()
        port.write(b"r")
        completed = 0
        passed = False
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            line = port.readline().decode("utf-8", errors="replace")
            if not line:
                continue
            print(line, end="", flush=True)
            if log:
                log.write(line)
                log.flush()
            if "FAIL:" in line or "Guru Meditation" in line or "assert failed" in line:
                return 1
            if "PASS: Opus + SD" in line:
                passed = True
            if "Send 'r'" in line and passed:
                completed += 1
                if completed == args.runs:
                    print(f"Verified {completed} successful round trips.")
                    return 0
                passed = False
                deadline = time.monotonic() + 60
                port.write(b"r")
        print("FAIL: timed out waiting for codec + SD test", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
