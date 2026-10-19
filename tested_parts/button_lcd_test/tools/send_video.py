"""Copy video.lcdv to the onboard SD card, verify it, and start playback."""

import argparse
import hashlib
from pathlib import Path
import struct
import time

import serial


def validate_video(data):
    if len(data) < 24:
        raise ValueError("Truncated video header")
    magic, width, height, fps, reserved, count, maximum = struct.unpack_from(
        "<8sHHHHII", data
    )
    if (magic != b"LCDV0001" or not 0 < width <= 240
            or not 0 < height <= 240 or not 0 < fps <= 30 or reserved
            or not count or not 4 <= maximum <= 32768):
        raise ValueError("Invalid video header")
    offset = 24
    for _ in range(count):
        if offset + 4 > len(data):
            raise ValueError("Missing frame length")
        length = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        if not 4 <= length <= maximum or offset + length > len(data):
            raise ValueError("Invalid frame length")
        jpeg = data[offset:offset + length]
        if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
            raise ValueError("Invalid JPEG markers")
        offset += length
    if offset != len(data):
        raise ValueError("Unexpected trailing data")
    return count, fps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("file", nargs="?", default="media/video.lcdv")
    args = parser.parse_args()
    data = Path(args.file).read_bytes()
    count, fps = validate_video(data)
    digest = hashlib.sha256(data).hexdigest()

    conn = serial.Serial(port=None, baudrate=921600, timeout=1,
                         write_timeout=15)
    conn.dtr = False
    conn.rts = False
    conn.port = args.port
    with conn:
        def send(payload):
            if conn.write(payload) != len(payload):
                raise IOError("Incomplete serial write")

        def wait_for(prefixes, timeout=30):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                line = conn.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                if line.startswith(("ERROR", "FAILED")):
                    raise RuntimeError(line)
                if line.startswith(prefixes):
                    return line
                print(line, flush=True)
            raise TimeoutError(f"No response: {prefixes}; reset the board")

        time.sleep(2)
        conn.reset_input_buffer()
        send(b"status\n")
        print(wait_for(("SD READY",)), flush=True)
        send(f"put {len(data)} {digest}\n".encode())
        reply = wait_for(("READY", "EXISTS verified"), 60)
        print(reply, flush=True)
        if reply == "READY":
            for offset in range(0, len(data), 4096):
                chunk = data[offset:offset + 4096]
                send(chunk)
                reply = wait_for(("ACK ",))
                if reply != f"ACK {offset + len(chunk)}":
                    raise RuntimeError(f"Wrong acknowledgement: {reply}")
                if offset % (4096 * 64) == 0:
                    print(f"Copied {offset + len(chunk):,}/{len(data):,} bytes",
                          flush=True)
            reply = wait_for(("SAVED ",), 60)
            if reply != f"SAVED {len(data)} {digest}":
                raise RuntimeError(f"Verification failed: {reply}")
            print(reply, flush=True)
        send(b"play\n")
        print(wait_for(("PLAYING ",)), flush=True)
        print(wait_for(("LOOP 1 ",), count / fps + 30), flush=True)
        send(b"status\n")
        print(wait_for(("VIDEO ",)), flush=True)
        print("Video verified on SD; one loop completed; playback continues.")


if __name__ == "__main__":
    main()
