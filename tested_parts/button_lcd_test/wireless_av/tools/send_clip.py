"""Copy an AV bundle to sender SD; press GPIO4 to send it over ESP-NOW."""

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
    default_file = Path(__file__).resolve().parents[2] / "media/clip.lcdav"
    parser.add_argument("file", nargs="?", default=str(default_file))
    args = parser.parse_args()
    data = Path(args.file).read_bytes()
    if len(data) < 32:
        raise ValueError("Missing AV header")
    magic, header, video_offset, audio_offset, samples, rate, size = (
        struct.unpack_from("<8s6I", data)
    )
    if (magic != b"LCDAV001" or header != 32 or audio_offset != 32
            or video_offset != 32 + samples or rate != 16000
            or not 0 < samples <= 4 * 1024 * 1024
            or size != len(data) - video_offset):
        raise ValueError("Invalid AV bundle")
    count, fps = validate_video(data[video_offset:])
    if abs(samples - round(count * rate / fps)) > 0:
        raise ValueError("Audio duration does not match video")
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
        print("AV clip verified on sender SD.")
        print("Press the sender's GPIO4 button to send it over ESP-NOW.")


if __name__ == "__main__":
    main()
