#!/usr/bin/env python3
"""Summarise radio_sniffer capture logs.

Reads ESPNOW and LORA lines (other lines are skipped; a prefix such as a
host timestamp is allowed) and prints, per radio and link: frames, unique
payloads, message types from the header guess, RSSI and frame rate.

    python3 tools/summarise.py capture.txt [more.txt ...] [--json]
"""

import argparse
import json
import re
import statistics
import sys
from collections import Counter, defaultdict

LINE = re.compile(r"\b(ESPNOW|LORA) (t=\S+(?: \S+=\S+)*)\s*$")


def parse_line(text):
    """Returns (radio, fields) for a capture line, else None."""
    match = LINE.search(text.rstrip("\r\n"))
    if not match:
        return None
    fields = {}
    for token in match.group(2).split(" "):
        key, _, value = token.partition("=")
        fields[key] = value
    try:
        fields["t"] = int(fields["t"])
        fields["len"] = int(fields["len"])
        fields["rssi"] = float(fields["rssi"])
    except (KeyError, ValueError):
        return None
    return match.group(1), fields


def link_of(radio, fields):
    """ESP-NOW: by MAC (board names kept). LoRa: by guessed IDs."""
    if radio == "ESPNOW":
        return f"{fields.get('src', '?')} > {fields.get('dst', '?')}"
    guess = fields.get("sm", "-")
    if guess != "-":
        parts = guess.split(":")
        if len(parts) == 4:
            return f"id {parts[2]}"
    return "unknown"


def type_of(fields):
    guess = fields.get("sm", "-")
    parts = guess.split(":")
    return parts[1] if guess != "-" and len(parts) == 4 else "-"


def summarise(lines):
    groups = defaultdict(list)
    for text in lines:
        parsed = parse_line(text)
        if parsed:
            radio, fields = parsed
            groups[(radio, link_of(radio, fields))].append(fields)
    rows = []
    for (radio, link), frames in sorted(groups.items()):
        times = [f["t"] for f in frames]
        rssi = [f["rssi"] for f in frames]
        span = (max(times) - min(times)) / 1e6
        with_data = [f["data"] for f in frames if "data" in f]
        row = {
            "radio": radio,
            "link": link,
            "frames": len(frames),
            "unique": len(set(with_data)) if with_data else None,
            "types": dict(Counter(type_of(f) for f in frames)),
            "rssi_min": min(rssi),
            "rssi_median": statistics.median(rssi),
            "rssi_max": max(rssi),
            "seconds": round(span, 3),
            "per_second": round((len(frames) - 1) / span, 2)
            if span > 0 else None,
        }
        if radio == "LORA":
            row["crc_bad"] = sum(1 for f in frames if f.get("crc") == "bad")
        rows.append(row)
    return rows


def print_table(rows, out):
    if not rows:
        print("no ESPNOW or LORA lines found", file=out)
        return
    for r in rows:
        types = ", ".join(f"{k} {v}" for k, v in sorted(r["types"].items()))
        unique = "?" if r["unique"] is None else r["unique"]
        rate = "-" if r["per_second"] is None else f"{r['per_second']}/s"
        extra = f", crc bad {r['crc_bad']}" if "crc_bad" in r else ""
        print(f"{r['radio']:6} {r['link']}\n"
              f"       {r['frames']} frames ({unique} unique{extra}) over "
              f"{r['seconds']} s, {rate}; rssi {r['rssi_min']:g} / "
              f"{r['rssi_median']:g} / {r['rssi_max']:g}; types: {types}",
              file=out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    lines = []
    for path in args.logs:
        with open(path, encoding="utf-8", errors="replace") as handle:
            lines.extend(handle)
    rows = summarise(lines)
    if args.json:
        json.dump(rows, sys.stdout, indent=2)
        print()
    else:
        print_table(rows, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
