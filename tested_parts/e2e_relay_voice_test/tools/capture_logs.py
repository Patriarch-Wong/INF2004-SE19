"""Capture several serial ports with shared timestamps, sending commands.

Each line is prefixed with seconds since the capture started. Opening a port
resets that board, so each port can open at its own time (LABEL=PORT@SECONDS)
and boards opened later boot after the earlier ones are running. --send
LABEL@SECONDS=TEXT writes TEXT and a newline to that board at that time; the
log records it as ">>> TEXT". Logs go to evidence/stage4/<run>_<label>.txt.

Example: open boards 1 and 2, then pair them over serial.
    ~/.platformio/penv/bin/python tools/capture_logs.py pair \\
        board1=/dev/cu.usbmodem5B420192981 \\
        board2=/dev/cu.usbmodem5B5F0215971@3 --duration 40 \\
        --send "board1@10=pair 2" --send "board2@10=pair 1"
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

EVIDENCE = Path(__file__).resolve().parent.parent / "evidence" / "stage4"


def port_argument(text: str) -> tuple:
    label, separator, rest = text.partition("=")
    if not separator or not label or not rest:
        raise argparse.ArgumentTypeError("expected LABEL=PORT[@SECONDS]")
    port, at, delay = rest.partition("@")
    try:
        opens = float(delay) if at else 0.0
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"bad open time: {delay}") from error
    return label, port, opens


def send_argument(text: str) -> tuple:
    target, separator, command = text.partition("=")
    label, at, when = target.partition("@")
    if not separator or not at or not label or not command:
        raise argparse.ArgumentTypeError("expected LABEL@SECONDS=TEXT")
    try:
        return label, float(when), command
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"bad send time: {when}") from error


def capture(label: str, port: str, start: float, open_at: float,
            stop_at: float, baud: int, sends: list, output: Path,
            errors: list) -> None:
    time.sleep(max(0.0, open_at - time.monotonic()))
    # Release DTR and RTS before opening: with pyserial's defaults a board on
    # a direct USB port was held in reset for the whole capture.
    link = serial.Serial()
    link.port = port
    link.baudrate = baud
    link.timeout = 0.1
    link.dtr = False
    link.rts = False
    pending_sends = sorted(sends)
    try:
        link.open()
        with link, output.open("w") as log:
            pending = b""
            while time.monotonic() < stop_at:
                now = time.monotonic() - start
                while pending_sends and pending_sends[0][0] <= now:
                    _, text = pending_sends.pop(0)
                    link.write(text.encode() + b"\n")
                    log.write(f"{now:6.2f} >>> {text}\n")
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
    parser.add_argument("run", help="run name, e.g. pair or reboot")
    parser.add_argument("ports", nargs="+", type=port_argument,
                        metavar="LABEL=PORT[@SECONDS]",
                        help="port to capture, opened at SECONDS (default 0)")
    parser.add_argument("--send", action="append", default=[],
                        type=send_argument, metavar="LABEL@SECONDS=TEXT",
                        help="write TEXT to a board at SECONDS; repeatable")
    parser.add_argument("--duration", type=float, default=60.0,
                        help="total capture seconds from start")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    labels = [label for label, _, _ in args.ports]
    if len(set(labels)) != len(labels):
        parser.error("labels must differ")
    for label, port, opens in args.ports:
        if not 0 <= opens < args.duration:
            parser.error(f"{label} must open within the capture")
    for label, when, _ in args.send:
        if label not in labels:
            parser.error(f"--send names unknown label {label}")
        opens = next(at for name, _, at in args.ports if name == label)
        if not opens <= when < args.duration:
            parser.error(f"{label} send at {when} s is outside its capture")

    EVIDENCE.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    stop_at = start + args.duration
    errors = []
    threads = []
    for label, port, opens in args.ports:
        output = EVIDENCE / f"{args.run}_{label}.txt"
        sends = [(when, text) for name, when, text in args.send
                 if name == label]
        print(f"{label}: {port} at {opens:.0f} s -> {output}")
        threads.append(threading.Thread(
            target=capture,
            args=(label, port, start, start + opens, stop_at, args.baud,
                  sends, output, errors)))
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for error in errors:
        print(f"ERROR {error}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
