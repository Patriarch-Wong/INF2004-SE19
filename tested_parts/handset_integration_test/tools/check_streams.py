"""Match every TX_END in the sender log with the RX_END in the receiver log.

A stream passes when the receiver played every frame the sender sent, the
CRC-32 over the played Opus packets equals the sender's, and the end
message arrived (intact=yes). A stream the sender gave up in a
push-to-talk collision (TX_YIELD) is never played, by design; it is listed
as yielded and does not fail the run. Prints JSON; exits 1 if any stream fails.

    python3 tools/check_streams.py evidence/run1_mic.txt evidence/run1_spk.txt
"""
import json
import re
import sys
from pathlib import Path

FIELD = re.compile(r"(\w+)=(\S+)")


def records(path: Path, tag: str) -> dict:
    """Stream ID -> fields for each `tag` line (log timestamps ignored)."""
    found = {}
    for line in path.read_text(errors="replace").splitlines():
        at = line.find(tag + " ")
        if at < 0:
            continue
        fields = dict(FIELD.findall(line[at + len(tag):]))
        if "stream" in fields:
            found[fields["stream"]] = fields
    return found


def yielded(path: Path) -> set:
    """Stream IDs the sender gave up in a collision (`TX_YIELD stream=X:`)."""
    return {stream.rstrip(":") for stream in records(path, "TX_YIELD")}


def check(sender: dict, receiver: dict, gave_up: set = frozenset()) -> dict:
    streams = []
    for stream, tx in sender.items():
        rx = receiver.get(stream)
        entry = {"stream": stream, "frames_sent": int(tx["frames"]),
                 "sender_crc": tx["crc"],
                 "nacks_received": int(tx["nacks"]),
                 "retransmits": int(tx["retransmits"]),
                 "send_errors": int(tx["send_errors"])}
        if stream in gave_up:
            entry.update(passed=True, yielded=True,
                         reason="sender yielded the floor; not played")
        elif rx is None:
            entry.update(passed=False, reason="no RX_END in receiver log")
        else:
            entry.update(
                played=int(rx["played"]), missing=int(rx["missing"]),
                direct=int(rx["direct"]), redundant=int(rx["redundant"]),
                retransmit=int(rx["retransmit"]), late=int(rx["late"]),
                receiver_crc=rx["crc"], underruns=int(rx["underruns"]),
                intact=rx["intact"] == "yes")
            for key in ("correlation", "snr_db", "worst_1s_snr_db"):
                if key in rx:
                    entry[key] = float(rx[key])
            reasons = []
            if entry["played"] != entry["frames_sent"]:
                reasons.append("not every frame played")
            if rx["crc"] != tx["crc"]:
                reasons.append("CRC differs")
            if not entry["intact"]:
                reasons.append("receiver did not report intact")
            entry.update(passed=not reasons, reason="; ".join(reasons))
        streams.append(entry)
    return {"streams": streams,
            "passed": bool(streams) and all(s["passed"] for s in streams)}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    sender = Path(sys.argv[1])
    result = check(records(sender, "TX_END"),
                   records(Path(sys.argv[2]), "RX_END"), yielded(sender))
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
