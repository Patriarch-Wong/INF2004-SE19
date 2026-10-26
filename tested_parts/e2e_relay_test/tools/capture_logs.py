"""Capture two serial ports at once with shared timestamps.

Each line is prefixed with seconds since the capture started. Opening a
port resets that board, so the second port is opened after --delay seconds:
the first board is already running when the second one boots. Logs are
written to evidence/stage3/<run>_<label>.txt.

Example (relay first, then board 1 after 12 s):

    ~/.platformio/penv/bin/python tools/capture_logs.py forward \\
        board3=/dev/cu.usbmodem5B420192791 \\
        board1=/dev/cu.usbmodem5B420192981 --delay 12 --duration 60
"""

import argparse
import sys
import threading
import time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run with ~/.platformio/penv/bin/python")

EVIDENCE = Path(__file__).resolve().parent.parent / "evidence" / "stage3"


def port_argument(text: str) -> tuple:
    label, separator, port = text.partition("=")
    if not separator or not label or not port:
        raise argparse.ArgumentTypeError("expected LABEL=PORT")
    return label, port


def capture(label: str, port: str, start: float, open_at: float,
            stop_at: float, baud: int, output: Path, errors: list) -> None:
    time.sleep(max(0.0, open_at - time.monotonic()))
    # Release DTR and RTS before opening: with pyserial's defaults a board on
    # a direct USB port was held in reset for the whole capture.
    link = serial.Serial()
    link.port = port
    link.baudrate = baud
    link.timeout = 0.1
    link.dtr = False
    link.rts = False
    try:
        link.open()
        with link, output.open("w") as log:
            pending = b""
            while time.monotonic() < stop_at:
                pending += link.read(1024)
                *lines, pending = pending.split(b"\n")
                for line in lines:
                    text = line.decode("utf-8", "replace").rstrip("\r")
                    log.write(f"{time.monotonic() - start:6.2f} {text}\n")
                log.flush()
    except (OSError, serial.SerialException) as error:
        errors.append(f"{label} ({port}): {error}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run", help="run name, e.g. forward or tamper")
    parser.add_argument("first", type=port_argument,
                        help="LABEL=PORT opened at time 0")
    parser.add_argument("second", type=port_argument,
                        help="LABEL=PORT opened after --delay")
    parser.add_argument("--delay", type=float, default=12.0,
                        help="seconds before opening the second port")
    parser.add_argument("--duration", type=float, default=60.0,
                        help="total capture seconds from start")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    if args.first[0] == args.second[0]:
        parser.error("the two labels must differ")
    if not 0 <= args.delay < args.duration:
        parser.error("--delay must be at least 0 and less than --duration")

    EVIDENCE.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    stop_at = start + args.duration
    errors = []
    threads = []
    for (label, port), open_at in ((args.first, start),
                                   (args.second, start + args.delay)):
        output = EVIDENCE / f"{args.run}_{label}.txt"
        print(f"{label}: {port} at {open_at - start:.0f} s -> {output}")
        threads.append(threading.Thread(
            target=capture,
            args=(label, port, start, open_at, stop_at, args.baud, output,
                  errors)))
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for error in errors:
        print(f"ERROR {error}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
