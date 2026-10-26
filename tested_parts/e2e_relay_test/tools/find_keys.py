"""Report which keys from include/e2e_keys.h appear in the given files.

Searches each file for every key as raw bytes (firmware images) and as hex
text in either case (serial logs). Use it on built firmware.bin files for
hardware test 4, and on evidence logs before committing them.
"""

import argparse
import re
import sys
from pathlib import Path

KEYS = Path(__file__).resolve().parent.parent / "include" / "e2e_keys.h"


def load_keys(path: Path) -> dict:
    """Returns {label: bytes}, labelled "network" or "pair a-b"."""
    keys = {}
    label = None
    collected = []
    for line in path.read_text().splitlines():
        if "network[16]" in line:
            label, collected = "network", []
            continue
        pair = re.search(r"// Pair (\d+-\d+)", line)
        if pair:
            label, collected = f"pair {pair.group(1)}", []
            continue
        if label is None:
            continue
        collected += [
            int(byte, 16) for byte in re.findall(r"0x([0-9A-Fa-f]{2})", line)
        ]
        if len(collected) == 16:
            keys.setdefault(label, bytes(collected))
            label = None
    return keys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--keys", type=Path, default=KEYS,
                        help="key header (default include/e2e_keys.h)")
    args = parser.parse_args()
    keys = load_keys(args.keys)
    if not keys:
        print(f"No keys found in {args.keys}")
        return 2
    for path in args.files:
        data = path.read_bytes()
        found = [
            label for label, key in keys.items()
            if key in data or key.hex().encode() in data.lower()
        ]
        print(f"{path}: {', '.join(found) if found else 'no keys'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
