#!/usr/bin/env python3
"""Audit the saved run3 trace; replay its counters through the real window.

This is specific to run3's single shared session after staggered startup.
It does not reproduce the radio duplication or revalidate cryptography:
the capture contains metadata, not the encrypted frames.
"""
import json
import re
import subprocess
import tempfile
from collections import Counter, defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def rows(name, pattern, after):
    path = ROOT / "evidence/relay" / f"run3_rate6_{name}.txt"
    result = []
    for line in path.read_text().splitlines():
        match = re.fullmatch(r"\s*(\d+\.\d+) " + pattern, line)
        if match and float(match[1]) >= after:
            result.append((float(match[1]), match.groups()[1:]))
    return result


def main():
    # Board 2 had an earlier session before board 1 rebooted. Exclude it;
    # comparing counters across sessions would falsely flag counters 1/2.
    sessions = rows("board1", r"SESSION established with 2 .*", 0)
    assert len(sessions) == 1, "Expected one board-1 session"
    start = sessions[0][0]
    tx = rows("board2", r"TX_DATA ([12]) 2 1 (\d+) \d+ 0", start)
    fwd = rows("relay", r"FWD ([12]) 2 1 (\d+) (\d+) ok \d+", start)
    rx = rows("board1", r"RX_DATA ([12]) 2 (\d+) \d+", start)
    rejects = rows("board1", r"REJECT \S+ \| (\d+) bytes \| replay", start)
    tx_counts = Counter(values for _, values in tx)
    rx_counts = Counter(values for _, values in rx)
    forwards = defaultdict(list)
    for time, (kind, counter, size) in fwd:
        forwards[(kind, counter)].append((time, size))
    duplicates = {key: events for key, events in forwards.items()
                  if len(events) > 1}
    assert len(duplicates) == len(rejects) == 10
    details = []
    for key, events in duplicates.items():
        assert tx_counts[key] == 1 and rx_counts[key] == 1
        assert len(events) == 2
        # Logs are host-read timestamps from independently buffered ports.
        # Require one nearby rejection of the same size, not exact ordering.
        matches = [(time, size) for time, (size,) in rejects
                   if size == events[0][1]
                   and min(abs(time - t) for t, _ in events) < 0.25]
        assert len(matches) == 1
        details.append({"type": int(key[0]), "counter": int(key[1]),
                        "sender_transmissions": tx_counts[key],
                        "relay_forwards": len(events),
                        "receiver_accepts": rx_counts[key],
                        "reject_time": matches[0][0]})
    assert len({item["reject_time"] for item in details}) == 10

    # Exercise the same C++ window used by session::link::finish, without
    # copying its algorithm into Python. Compare with duplicates removed.
    source = r'''
#include <iostream>
#include "replay_window.h"
int main()
{
    replay_window window;
    uint32_t counter = 0;
    while (std::cin >> counter)
    {
        if (window.is_fresh(counter))
        {
            window.accept(counter);
        }
        else
        {
            std::cout << counter << '\n';
        }
    }
}
'''
    with tempfile.TemporaryDirectory(prefix="relay-replay-") as folder:
        cpp = Path(folder) / "replay.cpp"
        binary = Path(folder) / "replay"
        cpp.write_text(source)
        subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "include"), str(cpp), "-o",
                        str(binary)], check=True)
        counters = [values[1] for _, values in fwd]

        def replay(sequence):
            output = subprocess.run([str(binary)],
                                    input="\n".join(sequence) + "\n",
                                    text=True, capture_output=True, check=True)
            return [int(value) for value in output.stdout.split()]

        rejected = replay(counters)
        assert sorted(rejected) == sorted(int(key[1]) for key in duplicates)
        assert replay(dict.fromkeys(counters)) == []
    print(json.dumps({"audit": "PASS", "session_start": start,
                      "replayed_rejections": len(rejected),
                      "deduplicated_rejections": 0,
                      "duplicates": details}, indent=2))


if __name__ == "__main__":
    main()
