#!/usr/bin/env python3
"""Save one snapshot per frame size over Wi-Fi (wifi_enabled = true).

Join the board's Wi-Fi ("ov5640-test", password "camtest123") first, then:

    python3 tools/grab.py                 # every size, into captures/
    python3 tools/grab.py --sizes 1 6 9   # VGA, UXGA, QSXGA only

Each file is checked for SOI/EOI markers and its SOF width and height, so a
PASS means the board sent a complete JPEG of the requested size.
"""

import argparse
import pathlib
import struct
import sys
import time
import urllib.request

SIZES = [
    ("QVGA", 320, 240), ("VGA", 640, 480), ("SVGA", 800, 600),
    ("XGA", 1024, 768), ("HD", 1280, 720), ("SXGA", 1280, 1024),
    ("UXGA", 1600, 1200), ("FHD", 1920, 1080), ("QXGA", 2048, 1536),
    ("QSXGA", 2560, 1920),
]


def jpeg_size(data):
    """Return (width, height) from the SOF header, or None if malformed."""
    if len(data) < 4 or data[:2] != b"\xff\xd8":
        return None
    if b"\xff\xd9" not in data[-16:]:
        return None
    i = 2
    while i + 4 <= len(data):
        if data[i] != 0xFF:
            return None
        marker = data[i + 1]
        if marker == 0xFF:
            i += 1
            continue
        if marker == 0x01 or 0xD0 <= marker <= 0xD8:
            i += 2
            continue
        if marker == 0xDA:
            return None
        (seg_len,) = struct.unpack(">H", data[i + 2:i + 4])
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            height, width = struct.unpack(">HH", data[i + 5:i + 9])
            return width, height
        i += 2 + seg_len
    return None


def get(url, timeout):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return resp.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--sizes", type=int, nargs="*",
                        default=list(range(len(SIZES))))
    parser.add_argument("--out", default="captures")
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    base = f"http://{args.host}"
    failures = 0
    for index in args.sizes:
        name, want_w, want_h = SIZES[index]
        try:
            get(f"{base}/control?size={index}", args.timeout)
            time.sleep(0.5)  # let the sensor switch size
            started = time.monotonic()
            data = get(f"{base}/capture", args.timeout)
            elapsed = time.monotonic() - started
        except OSError as exc:
            print(f"{name:5} FAIL request: {exc}")
            failures += 1
            continue
        size = jpeg_size(data)
        ok = size == (want_w, want_h)
        path = out / f"{stamp}_{index}_{name}.jpg"
        path.write_bytes(data)
        got = f"{size[0]}x{size[1]}" if size else "corrupt"
        print(f"{name:5} {'PASS' if ok else 'FAIL'} {got:>9} "
              f"{len(data) / 1024:7.1f} KB {elapsed:5.2f} s  {path}")
        failures += 0 if ok else 1
    get(f"{base}/control?size=1", args.timeout)
    print(f"{'PASS' if failures == 0 else 'FAIL'}: "
          f"{len(args.sizes) - failures}/{len(args.sizes)} sizes")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
