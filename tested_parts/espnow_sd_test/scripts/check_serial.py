#!/usr/bin/env python3
"""Require receiver SD read-back PASS and sender completion ACK PASS."""
import argparse
from contextlib import ExitStack
from pathlib import Path
import sys
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", nargs=2, required=True,
                        metavar=("RECEIVER", "SENDER"))
    parser.add_argument("--seconds", type=float, default=90)
    parser.add_argument("--output", default="serial_test.log")
    args = parser.parse_args()
    if args.seconds <= 0 or len(set(args.ports)) != 2:
        parser.error("Use two distinct ports and a positive duration")
    with ExitStack() as stack:
        log = stack.enter_context(Path(args.output).open("w"))
        ports = []
        for name in args.ports:
            port = serial.Serial(baudrate=115200, timeout=0.1)
            port.dtr = False
            port.rts = False
            port.port = name
            port.open()
            ports.append(stack.enter_context(port))
        buffers = [b"", b""]
        passed = [False, False]
        deadline = time.monotonic() + args.seconds
        print("Monitoring both boards; press RESET without BOOT on both.",
              flush=True)
        while time.monotonic() < deadline:
            for index, port in enumerate(ports):
                buffers[index] += port.read(max(1, port.in_waiting))
                while b"\n" in buffers[index]:
                    raw, buffers[index] = buffers[index].split(b"\n", 1)
                    line = raw.decode("utf-8", errors="replace").strip()
                    text = f"[{args.ports[index]}] {line}"
                    print(text, flush=True)
                    log.write(text + "\n")
                    log.flush()
                    if any(token in line for token in (
                        "STOPPED", "TX FAIL", "verification failed",
                        "Guru Meditation", "assert failed", "mismatch",
                    )):
                        return 1
                    expected = (
                        "Status | receiver | RX PASS (4096/4096)",
                        "Status | sender | TX PASS",
                    )
                    if expected[index] in line:
                        passed[index] = True
            if all(passed):
                text = "PASS: 4096 bytes sent from RAM, saved to SD and read back"
                print(text, flush=True)
                log.write(text + "\n")
                return 0
        print("FAIL: timed out waiting for both boards", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
