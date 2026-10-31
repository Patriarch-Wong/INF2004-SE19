"""Stream video.lcdv frame by frame to the LCD over serial; no SD card needed."""

import argparse
from pathlib import Path
import struct
import time

import serial

from send_video import validate_video


def read_frames(data):
    count, fps = validate_video(data)
    frames = []
    offset = 24
    for _ in range(count):
        length = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        frames.append(data[offset:offset + length])
        offset += length
    return frames, fps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--loops", type=int, default=1)
    parser.add_argument("file", nargs="?", default="media/video.lcdv")
    args = parser.parse_args()
    frames, fps = read_frames(Path(args.file).read_bytes())
    period = 1 / fps

    conn = serial.Serial(port=None, baudrate=921600, timeout=5,
                         write_timeout=15)
    conn.dtr = False
    conn.rts = False
    conn.port = args.port
    with conn:
        def wait_for(prefix):
            while True:
                line = conn.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    raise TimeoutError(f"No response: {prefix}; reset the board")
                if line.startswith(("ERROR", "FAILED")):
                    raise RuntimeError(line)
                if line.startswith(prefix):
                    return line
                print(line, flush=True)

        time.sleep(2)
        conn.reset_input_buffer()
        conn.write(b"status\n")
        print(wait_for("STREAM "), flush=True)

        sent = skipped = 0
        outstanding = 0
        slowest_us = 0
        started = time.monotonic()
        for loop in range(args.loops):
            for index, jpeg in enumerate(frames):
                due = started + (loop * len(frames) + index) * period
                now = time.monotonic()
                # Skip overdue frames instead of accumulating playback delay.
                if now - due >= period:
                    skipped += 1
                    continue
                if due > now:
                    time.sleep(due - now)
                # At most two frames in flight: one decoding, one queued.
                if outstanding == 2:
                    slowest_us = max(slowest_us,
                                     int(wait_for("SHOWN ").split()[2]))
                    outstanding -= 1
                conn.write(f"frame {len(jpeg)}\n".encode() + jpeg)
                sent += 1
                outstanding += 1
            print(f"LOOP {loop + 1} sent={sent} skipped={skipped}", flush=True)
        while outstanding:
            slowest_us = max(slowest_us, int(wait_for("SHOWN ").split()[2]))
            outstanding -= 1
        elapsed = time.monotonic() - started
        print(f"Done: {sent} frames shown, {skipped} skipped, "
              f"{elapsed:.1f} s, slowest decode {slowest_us} us", flush=True)


if __name__ == "__main__":
    main()
