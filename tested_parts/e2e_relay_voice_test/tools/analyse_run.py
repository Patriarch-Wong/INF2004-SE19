#!/usr/bin/env python3
"""Analyse one two-way relay run from the three boards' capture logs.

Every log line starts with the capture tool's shared timestamp (seconds), so
streams and relay lines can be compared across boards. Prints a JSON report
and exits 1 if any check fails.

    python3 tools/analyse_run.py --board1 evidence/relay/run1_board1.txt \\
        --board2 evidence/relay/run1_board2.txt \\
        --relay evidence/relay/run1_relay.txt > evidence/relay/run1.json
"""
import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

FRAMES = 3000
LIVE_DEPTH = 200000
MIN_DELIVERY = 0.99
MAX_LATE_LOST = 0.01
MIN_CORR = 0.99
MIN_SNR_DB = 20.0
MAX_CAPTURE_TO_SEND_P99_US = 20000
TYPE_TEXT = 1
TYPE_VOICE = 2

LINE = re.compile(r"^\s*(\d+\.\d+) (.*)$")
TX_START = re.compile(r"STREAM_TX_START ([0-9A-F]{8}) to (\d+) frames (\d+) (\d)")
OPUS_TX = re.compile(r"OPUS_TX (\d+) (\d+) ([0-9A-F]{8}) (\d+) (\d+) (-?\d+) (\d)")
TX_REPORT = re.compile(
    r"TX_REPORT ([0-9A-F]{8}) to (\d+) frames (\d+) sent (\d+) "
    r"send_errors (\d+) build_errors (\d+) echoes (\d+)")
TX_DATA = re.compile(r"TX_DATA (\d+) (\d+) (\d+) (\d+) (-?\d+) (-?\d+)")
RX_DATA = re.compile(r"RX_DATA (\d+) (\d+) (\d+) (-?\d+)")
RTT = re.compile(r"RTT (\d+) (\d+)")
LOCK = re.compile(r"STREAM_LOCK ([0-9A-F]{8}) from (\d+) frames (\d+) anchor (\d+)")
OPUS_RX = re.compile(r"OPUS_RX (\d+) (\d+) ([0-9A-F]{8}) (\d+) (\d+)")
OPUS_PLAY = re.compile(r"OPUS_PLAY (\d+) (\d+) (\d+) (-?\d+)")
RX_REPORT = re.compile(
    r"RX_REPORT ([0-9A-F]{8}) from (\d+) anchor (\d+) frames (\d+) "
    r"received (\d+) duplicates (\d+) reordered (\d+) foreign (\d+) "
    r"stale (\d+) wrong_frames (\d+) played (\d+) abandoned (\d)")
RX_END = re.compile(r"RX_REPORT_END ([0-9A-F]{8})")
PLAYOUT = re.compile(r"PLAYOUT (\d+) on_time (\d+) late (\d+) lost (\d+) jitter (\d+)")
QUALITY = re.compile(
    r"QUALITY (\w+) (\d+) decoded (\d+) concealed (\d+) decode_failed (\d+) "
    r"failed (\d+) corr (-?[\d.]+) snr (-?[\d.]+) worst (-?[\d.]+) windows (\d+)")
LIVE_MISSED = re.compile(r"LIVE_MISSED (\d+)")
DROPS = re.compile(
    r"VOICE_DROPS timer (\d+) radio (\d+) encode (\d+) codec (\d+) play_log (\d+)")
SESSION = re.compile(r"SESSION established with (\d+) \| session ([0-9A-F]{8})")
FWD = re.compile(r"FWD (\d+) (\d+) (\d+) (\d+) (\d+) (\S+) (\d+)")
RADIO = re.compile(r"RADIO channel (\d+) rate (\d+) (\w+)")
RELAY_STATUS = re.compile(
    r"Status \| forwarded (\d+) \| no ack (\d+) \| send errors (\d+) \| "
    r"dropped (\d+) \| pairing ignored (\d+) \| queue drops (\d+) \| "
    r"unknown MAC drops (\d+)")


def read_lines(path):
    lines = []
    for raw in Path(path).read_text(errors="replace").splitlines():
        match = LINE.match(raw)
        if match:
            lines.append((float(match.group(1)), match.group(2)))
    return lines


def summary(values):
    """Nearest-rank summary, as the firmware's voice::summarise."""
    if not values:
        return {"count": 0}
    ordered = sorted(values)

    def rank(p):
        index = max(1, -(-len(ordered) * p // 100)) - 1
        return ordered[int(index)]

    return {"count": len(ordered), "min": ordered[0], "median": rank(50),
            "p99": rank(99), "max": ordered[-1]}


def parse_handset(path, board):
    """Streams sent and received, data frames, sessions and drops."""
    log = {"board": board, "sent": {}, "received": {}, "tx_data": [],
           "rx_data": [], "sessions": set(), "drops": None,
           "not_via_relay": 0, "rejects": Counter(), "booted": False,
           "radio": []}
    tx = rx = None
    for time, text in read_lines(path):
        if text.startswith("Build mode: encrypted handset via relay"):
            log["booted"] = True
        if (m := RADIO.match(text)):
            log["radio"].append([time, int(m[1]), int(m[2]), m[3]])
        elif (m := TX_START.match(text)):
            tx = {"id": m[1], "to": int(m[2]), "start": time, "end": None,
                  "ok": m[4] == "1", "crc": {}, "send_failed": 0,
                  "encode_us": [], "generate_us": [], "capture_to_send_us": [],
                  "rtt_us": [], "report": None}
            log["sent"][m[1]] = tx
        elif (m := OPUS_TX.match(text)) and tx and tx["end"] is None:
            seq = int(m[1])
            if m[7] == "1":
                tx["crc"][seq] = m[3]
            else:
                tx["send_failed"] += 1
            tx["generate_us"].append(int(m[4]))
            tx["encode_us"].append(int(m[5]))
            tx["capture_to_send_us"].append(int(m[6]))
        elif (m := RTT.match(text)) and tx and tx["end"] is None:
            tx["rtt_us"].append(int(m[2]))
        elif (m := TX_REPORT.match(text)):
            stream = log["sent"].get(m[1])
            if stream and stream["report"] is None:
                stream["end"] = time
                stream["report"] = {
                    "sent": int(m[4]), "send_errors": int(m[5]),
                    "build_errors": int(m[6]), "echoes": int(m[7])}
        elif (m := LOCK.match(text)):
            rx = {"id": m[1], "from": int(m[2]), "start": time, "end": None,
                  "anchor": int(m[4]), "crc": {}, "outcomes": Counter(),
                  "decode_us": [], "lag_us": [], "playout": {},
                  "quality": {}, "live_missed": None, "report": None}
            log["received"][m[1]] = rx
        elif (m := OPUS_RX.match(text)) and rx and rx["report"] is None:
            if m[5] == "0":  # voice::record_result::stored
                rx["crc"][int(m[1])] = m[3]
        elif (m := OPUS_PLAY.match(text)) and rx and rx["report"] is None:
            rx["outcomes"][int(m[2])] += 1
            rx["decode_us"].append(int(m[3]))
            rx["lag_us"].append(int(m[4]))
        elif (m := RX_REPORT.match(text)):
            stream = log["received"].get(m[1])
            if stream and stream["report"] is None:
                stream["report"] = {
                    "received": int(m[5]), "duplicates": int(m[6]),
                    "reordered": int(m[7]), "foreign": int(m[8]),
                    "stale": int(m[9]), "wrong_frames": int(m[10]),
                    "played": int(m[11]), "abandoned": m[12] == "1"}
                rx = stream
                rx["_reporting"] = True
        elif (m := PLAYOUT.match(text)) and rx and rx.get("_reporting"):
            rx["playout"][int(m[1])] = {
                "on_time": int(m[2]), "late": int(m[3]), "lost": int(m[4]),
                "jitter_us": int(m[5])}
        elif (m := QUALITY.match(text)) and rx and rx.get("_reporting"):
            rx["quality"][m[1]] = {
                "decoded": int(m[3]), "concealed": int(m[4]),
                "decode_failed": int(m[5]), "failed": int(m[6]),
                "corr": float(m[7]), "snr_db": float(m[8]),
                "worst_window_db": float(m[9])}
        elif (m := LIVE_MISSED.match(text)) and rx and rx.get("_reporting"):
            rx["live_missed"] = int(m[1])
        elif (m := RX_END.match(text)):
            stream = log["received"].get(m[1])
            if stream and stream.get("_reporting"):
                stream["end"] = time
                stream.pop("_reporting")
        elif (m := TX_DATA.match(text)):
            log["tx_data"].append((int(m[1]), int(m[2]), int(m[3]),
                                   int(m[4]), int(m[6])))
        elif (m := RX_DATA.match(text)):
            log["rx_data"].append((int(m[1]), int(m[2]), board, int(m[3])))
        elif (m := SESSION.match(text)):
            log["sessions"].add((int(m[1]), m[2]))
        elif (m := DROPS.match(text)):
            log["drops"] = {"timer": int(m[1]), "radio": int(m[2]),
                            "encode": int(m[3]), "codec": int(m[4]),
                            "play_log": int(m[5])}
        elif text.startswith("REJECT "):
            reason = text.rsplit("| ", 1)[-1]
            log["rejects"][reason] += 1
            if reason == "not via the relay":
                log["not_via_relay"] += 1
    return log


def parse_relay(path):
    relay = {"forwards": [], "status": None, "drops": Counter(),
             "no_pair_keys": False, "booted": False, "radio": []}
    for time, text in read_lines(path):
        if text.startswith("Build mode: relay"):
            relay["booted"] = True
        if "Pair keys held: 0" in text:
            relay["no_pair_keys"] = True
        if (m := RADIO.match(text)):
            relay["radio"].append([time, int(m[1]), int(m[2]), m[3]])
        elif (m := FWD.match(text)):
            relay["forwards"].append({
                "time": time, "type": int(m[1]), "src": int(m[2]),
                "dst": int(m[3]), "counter": int(m[4]), "size": int(m[5]),
                "result": m[6], "relay_us": int(m[7])})
        elif (m := RELAY_STATUS.match(text)):
            relay["status"] = {
                "forwarded": int(m[1]), "no_ack": int(m[2]),
                "send_errors": int(m[3]), "dropped": int(m[4]),
                "pairing_ignored": int(m[5]), "queue_drops": int(m[6]),
                "unknown_mac_drops": int(m[7])}
        elif text.startswith("DROP "):
            relay["drops"][text.rsplit("| ", 1)[-1]] += 1
    return relay


def overlaps(a, b):
    return a["start"] < (b["end"] or float("inf")) and \
        b["start"] < (a["end"] or float("inf"))


def stream_result(tx, receiver, sender_board, relay):
    """One stream, sender to receiver, through the relay."""
    rx = receiver["received"].get(tx["id"])
    result = {"id": tx["id"], "from": sender_board, "to": tx["to"],
              "start_s": tx["start"], "end_s": tx["end"],
              "sent": len(tx["crc"]), "send_failed": tx["send_failed"],
              "tx_report": tx["report"],
              "sender_timing_us": {
                  "generate": summary(tx["generate_us"]),
                  "encode": summary(tx["encode_us"]),
                  "capture_to_send": summary(tx["capture_to_send_us"])},
              "rtt_us": summary(tx["rtt_us"])}
    window = [f for f in relay["forwards"] if f["type"] == TYPE_VOICE and
              f["src"] == sender_board and f["dst"] == tx["to"] and
              tx["start"] <= f["time"] <= (tx["end"] or float("inf")) + 1]
    result["relay"] = {
        "voice_forwards": len(window),
        "results": dict(Counter(f["result"] for f in window)),
        "relay_us": summary([f["relay_us"] for f in window
                             if f["result"] == "ok"])}
    if rx is None or rx["report"] is None:
        result["received"] = 0
        result["pass"] = False
        result["failures"] = ["receiver never reported this stream"]
        return result
    matched = sum(1 for seq, crc in rx["crc"].items()
                  if tx["crc"].get(seq) == crc)
    mismatched = sum(1 for seq, crc in rx["crc"].items()
                     if seq in tx["crc"] and tx["crc"][seq] != crc)
    unsent = sum(1 for seq in rx["crc"] if seq not in tx["crc"])
    live = rx["playout"].get(LIVE_DEPTH, {})
    late_lost = live.get("late", FRAMES) + live.get("lost", FRAMES)
    in_order = rx["quality"].get("in_order", {})
    result.update({
        "received": len(rx["crc"]), "crc_matched": matched,
        "crc_mismatched": mismatched, "received_not_sent": unsent,
        "rx_report": rx["report"], "playout": rx["playout"],
        "quality": rx["quality"], "live_missed": rx["live_missed"],
        "play_outcomes": {str(k): v for k, v in sorted(rx["outcomes"].items())},
        "decode_us": summary(rx["decode_us"]),
        "play_lag_us": summary(rx["lag_us"])})
    failures = []
    if len(rx["crc"]) < MIN_DELIVERY * FRAMES:
        failures.append(f"delivered {len(rx['crc'])} of {FRAMES}")
    if mismatched or unsent:
        failures.append(f"{mismatched} CRC mismatches, {unsent} never sent")
    if late_lost >= MAX_LATE_LOST * FRAMES:
        failures.append(f"{late_lost} late or lost at {LIVE_DEPTH // 1000} ms")
    if in_order.get("corr", 0) <= MIN_CORR or \
            in_order.get("snr_db", 0) <= MIN_SNR_DB:
        failures.append("in-order audio below corr 0.99 / 20 dB")
    if rx["live_missed"] != 0:
        failures.append(f"live missed {rx['live_missed']}")
    if rx["report"]["abandoned"]:
        failures.append("stream abandoned")
    result["pass"] = not failures
    result["failures"] = failures
    return result


def analyse(board1_path, board2_path, relay_path):
    handsets = {1: parse_handset(board1_path, 1),
                2: parse_handset(board2_path, 2)}
    relay = parse_relay(relay_path)
    checks = {}

    # R1: every data frame a handset accepted was forwarded by the relay,
    # and none came straight from the other handset.
    # Any forward counts: a frame can arrive even when its ACK is lost.
    forwarded = Counter((f["type"], f["src"], f["dst"], f["counter"])
                        for f in relay["forwards"])
    accepted = Counter(frame for h in handsets.values()
                       for frame in h["rx_data"])
    not_forwarded = sum((accepted - forwarded).values())
    direct = sum(h["not_via_relay"] for h in handsets.values())
    checks["R1_relay_path"] = {
        "accepted_frames": sum(accepted.values()),
        "accepted_without_relay_forward": not_forwarded,
        "rejected_not_via_relay": direct,
        "relay_holds_no_pair_key": relay["no_pair_keys"],
        "pass": bool(accepted) and not_forwarded == 0 and direct == 0 and
        relay["no_pair_keys"]}

    # R2: each handset established a session with the other and accepted
    # its text frames.
    sessions = {}
    for board, h in handsets.items():
        peer = 3 - board
        sessions[board] = {
            "sessions_with_peer": sorted(s for p, s in h["sessions"]
                                         if p == peer),
            "text_frames_accepted": sum(
                1 for t, src, _, _ in h["rx_data"]
                if t == TYPE_TEXT and src == peer)}
    shared = set(sessions[1]["sessions_with_peer"]) & \
        set(sessions[2]["sessions_with_peer"])
    checks["R2_sessions"] = {
        "per_handset": sessions, "shared_session_ids": sorted(shared),
        "pass": bool(shared) and all(s["text_frames_accepted"] > 0
                                     for s in sessions.values())}

    # R3 / R4: streams, alone and overlapping the other direction.
    streams = []
    for board, h in handsets.items():
        for tx in h["sent"].values():
            streams.append(stream_result(tx, handsets[tx["to"]], board, relay))
    for s in streams:
        s["two_way"] = any(o["from"] != s["from"] and overlaps(
            {"start": o["start_s"], "end": o["end_s"]},
            {"start": s["start_s"], "end": s["end_s"]}) for o in streams)
    one_way = [s for s in streams if not s["two_way"]]
    two_way = [s for s in streams if s["two_way"]]
    checks["R3_one_way"] = {
        "directions": sorted({f"{s['from']}->{s['to']}" for s in one_way}),
        "pass": {f"{s['from']}->{s['to']}" for s in one_way if s["pass"]} ==
        {"1->2", "2->1"} and all(s["pass"] for s in one_way)}
    checks["R4_two_way"] = {
        "directions": sorted({f"{s['from']}->{s['to']}" for s in two_way}),
        "pass": {f"{s['from']}->{s['to']}" for s in two_way if s["pass"]} ==
        {"1->2", "2->1"} and all(s["pass"] for s in two_way)}

    # R5: both handsets kept up while sending, receiving or both.
    pipelines = {}
    for board, h in handsets.items():
        c2s = [v for tx in h["sent"].values()
               for v in tx["capture_to_send_us"]]
        drops = h["drops"] or {}
        p99 = summary(c2s).get("p99", 0)
        pipelines[board] = {
            "drops": drops, "capture_to_send_us": summary(c2s),
            "pass": bool(drops) and not any(drops.values()) and
            p99 < MAX_CAPTURE_TO_SEND_P99_US}
    checks["R5_pipelines"] = {
        "per_handset": pipelines,
        "pass": all(p["pass"] for p in pipelines.values())}

    # R6: the relay kept up.
    status = relay["status"] or {}
    results = Counter(f["result"] for f in relay["forwards"])
    checks["R6_relay"] = {
        "status": status, "forward_results": dict(results),
        "drops": dict(relay["drops"]),
        "relay_us_ok": summary([f["relay_us"] for f in relay["forwards"]
                                if f["result"] == "ok"]),
        "pass": bool(status) and status.get("queue_drops", 1) == 0 and
        not relay["drops"]}

    return {"booted": {"board1": handsets[1]["booted"],
                       "board2": handsets[2]["booted"],
                       "relay": relay["booted"]},
            "radio": {"board1": handsets[1]["radio"],
                      "board2": handsets[2]["radio"],
                      "relay": relay["radio"]},
            "checks": checks, "streams": streams,
            "handset_rejects": {b: dict(h["rejects"])
                                for b, h in handsets.items()},
            "pass": all(c["pass"] for c in checks.values())}


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--board1", required=True)
    parser.add_argument("--board2", required=True)
    parser.add_argument("--relay", required=True)
    args = parser.parse_args()
    report = analyse(args.board1, args.board2, args.relay)
    json.dump(report, sys.stdout, indent=1)
    print()
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
