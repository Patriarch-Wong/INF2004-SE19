"""Record the handset microphone to a WAV file over serial.

Sends `micdump SECONDS`, reads the raw PCM16 block between DUMP_BEGIN and
DUMP_END, checks its CRC-32 and writes a 16 kHz mono WAV. The audio is what
Opus would encode: averaged ADC readings, bias removed, 300-3000 Hz, the
noise gate and gain applied. Opening the port resets the board, so the tool
waits for "Ready" first. The gate measures the room for the first second
after boot and is silent then, so stay quiet at the start.

    ~/.platformio/penv/bin/python tools/mic_dump.py \\
        --port /dev/cu.usbmodem5B420192981 --seconds 5 evidence/mic_run1.wav
"""
import argparse
import re
import sys
import time
import wave
import zlib
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run with ~/.platformio/penv/bin/python")

HEADER = re.compile(
    rb"DUMP_BEGIN rate=(\d+) samples=(\d+) bytes=(\d+) crc=([0-9A-F]{8})")


def read_line(port, deadline, log):
    line = port.readline()
    if line:
        log.write(line)
    if time.monotonic() > deadline:
        raise TimeoutError("timed out waiting for the board")
    return line


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", type=Path, help="WAV file to write")
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--seconds", type=int, default=5)
    parser.add_argument("--no-wait", action="store_true",
                        help="do not wait for Ready (port did not reset)")
    parser.add_argument("--command", action="append", default=[],
                        help="extra command to send first, e.g. 'gain 4'")
    args = parser.parse_args()

    log_path = args.output.with_suffix(".log")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with serial.Serial(args.port, args.baud, timeout=0.5) as port, \
            open(log_path, "wb") as log:
        if not args.no_wait:
            deadline = time.monotonic() + 30
            while b"Ready" not in read_line(port, deadline, log):
                pass
        for command in args.command:
            port.write(command.encode() + b"\n")
            time.sleep(0.2)
        port.write(f"micdump {args.seconds}\n".encode())
        deadline = time.monotonic() + args.seconds + 30
        match = None
        while not match:
            match = HEADER.search(read_line(port, deadline, log))
        rate, samples, size = (int(match.group(i)) for i in (1, 2, 3))
        expected_crc = int(match.group(4), 16)
        data = bytearray()
        while len(data) < size:
            chunk = port.read(size - len(data))
            if not chunk and time.monotonic() > deadline:
                raise TimeoutError(f"got {len(data)} of {size} bytes")
            data.extend(chunk)
        while b"DUMP_END" not in read_line(port, deadline, log):
            pass

    crc = zlib.crc32(data) & 0xFFFFFFFF
    if crc != expected_crc:
        print(f"CRC mismatch: board {expected_crc:08X}, received {crc:08X}")
        return 1
    with wave.open(str(args.output), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(bytes(data))
    values = memoryview(bytes(data)).cast("h")
    peak = max(abs(v) for v in values)
    clipped = sum(1 for v in values if abs(v) >= 32700)
    rms = (sum(v * v for v in values) / len(values)) ** 0.5
    print(f"Wrote {args.output}: {samples} samples at {rate} Hz, "
          f"CRC {crc:08X} ok, rms {rms:.0f}, peak {peak}, "
          f"clipped {clipped}; serial log {log_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
