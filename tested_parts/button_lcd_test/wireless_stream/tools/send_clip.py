"""Copy an H.264/AAC stream to sender SD; press GPIO4 to transmit."""

import argparse
import hashlib
from pathlib import Path
from encode_stream import validate
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    default_file = Path(__file__).resolve().parents[2] / "media/clip.lcdstream"
    parser.add_argument("file", nargs="?", default=str(default_file))
    args = parser.parse_args()
    clip_path = Path(args.file).resolve()
    data = clip_path.read_bytes()
    frames, duration = validate(data)
    digest = hashlib.sha256(data).hexdigest()
    print(f"File: {clip_path}", flush=True)
    print(f"Size: {len(data):,} bytes; {frames} frames; {duration:.3f} s",
          flush=True)

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
        send(b"identity\n")
        try:
            identity = wait_for(("FIRMWARE ",), 5)
        except (RuntimeError, TimeoutError) as error:
            raise RuntimeError(
                "Cannot identify streaming sender firmware. Flash the "
                "current wireless_stream sender build, press RESET, "
                "and retry. No clip was uploaded."
            ) from error
        if identity != "FIRMWARE wireless_stream sender":
            raise RuntimeError(
                f"Wrong firmware/role: {identity}; connect the streaming "
                "sender. No clip was uploaded."
            )
        print(identity, flush=True)
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
        print("H264/AAC stream verified on sender SD.")
        print("Press the sender's GPIO4 button to send it over ESP-NOW.")


if __name__ == "__main__":
    main()
