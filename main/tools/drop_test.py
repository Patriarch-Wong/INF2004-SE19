"""Dropped-frame test for the boards plugged in (two or more), over serial.

For every pair of boards it connects them over the flood mesh and sends
test-signal voice streams both ways, then counts, from the sender's TX_END
and the receiver's RX_END, how many 20 ms frames:

    arrived the first time             (direct)
    were rebuilt from the next packet  (redundant)
    came back after a NACK             (retransmit)
    came too late to play              (late)
    were never played                  (missing: a hole in the audio)

Each pair is tested three ways (two with only two boards):

    all     every board on, as in normal use
    direct  the other boards muted (`flood mute on`): no relay can help
    relay   the pair block each other (`relay block N`): every frame must
            go through another board

The script reboots every board first and reads its ID from the boot
banner, so the ports may be given in any order. Logs go to
evidence/<run>_board<N>.txt and the numbers to evidence/<run>_summary.json.

    ~/.platformio/penv/bin/python tools/drop_test.py drop1 PORT PORT [PORT]
        [--seconds 30] [--rounds 1] [--modes all,direct,relay]
"""
import argparse
import itertools
import json
import re
import sys
import time

import two_board_test
from lora_edge_test import EVIDENCE, Board
from two_board_test import connect_over, disconnect_over, ui_line

FIELD = re.compile(r"(\w+)=(\S+)")
TAG = "FLOOD"
RX_COUNTS = ("played", "missing", "direct", "redundant", "retransmit", "late",
             "duplicate", "nacks", "underruns")


def say(text):
    print(text, flush=True)


def fields(match):
    return dict(FIELD.findall(match.string)) if match else {}


def identify(board):
    """The board's ID and channel, from the banner after its reboot."""
    match = board.wait(r"Device (\d+) \(handset\), channel (\d+)", 15, 0)
    if not match:
        return None, None
    return int(match.group(1)), int(match.group(2))


def wait_idle(*boards):
    two_board_test.wait_idle(boards, "menu|flood|lora|status|tools")


def wait_online(board, peer, since, timeout=30):
    return bool(board.wait(rf"{TAG} PEER {peer} \(.*\) online", timeout,
                           since))


def stream(sender, receiver, sender_id, receiver_id, seconds):
    """Sends one test stream and returns what both ends counted."""
    s_tx, s_rx = sender.mark(), receiver.mark()
    sender.send(f"test {seconds}")
    start = sender.wait(r"TX_START stream=(\w+) .* via=flood", 4, s_tx)
    row = {"from": sender_id, "to": receiver_id, "started": bool(start)}
    if not start:
        return row
    name = start.group(1)
    row["stream"] = name
    row["redundancy"] = fields(start).get("redundancy", "?")
    heard = receiver.wait(rf"RX_START stream={name} ", 6, s_rx)
    row["heard"] = bool(heard)
    tx = fields(sender.wait(rf"TX_END stream={name} ", seconds + 8, s_tx))
    rx = fields(receiver.wait(rf"RX_END stream={name} ", 8, s_rx)
                if heard else None)
    row["sent"] = int(tx.get("frames", seconds * 50))
    for key in ("nacks", "retransmits", "send_errors", "encode_drops"):
        row[f"tx_{key}"] = int(tx.get(key, 0))
    if rx:
        for key in RX_COUNTS:
            row[key] = int(rx.get(key, 0))
        row["intact"] = rx.get("intact") == "yes"
        row["worst_1s_snr_db"] = float(rx.get("worst_1s_snr_db", "nan"))
    else:  # Nothing played: every frame is a hole.
        for key in RX_COUNTS:
            row[key] = 0
        row["missing"] = row["sent"]
        row["intact"] = False
    time.sleep(2.0)  # The sender lingers 1.5 s answering NACKs.
    return row


def print_row(mode, row):
    if not row.get("started"):
        say(f"    {row['from']} -> {row['to']}  did not start")
        return
    sent = row["sent"]
    first = row["direct"]
    say(f"    {row['from']} -> {row['to']}  sent {sent:4d}  first-time "
        f"{first:4d} ({100.0 * first / sent:5.1f}%)  redundant "
        f"{row['redundant']:3d}  retransmit {row['retransmit']:3d}  late "
        f"{row['late']:3d}  MISSING {row['missing']:3d}  "
        f"{'intact' if row['intact'] else 'NOT intact'}")


def run_pair(boards, a_id, b_id, mode, args):
    """One pair one way of testing; returns its stream rows."""
    a, b = boards[a_id], boards[b_id]
    others = [i for i in boards if i not in (a_id, b_id)]
    say(f"\n{a_id} <-> {b_id}, {mode}")
    for board in boards.values():
        board.note(f"=== pair {a_id}-{b_id} mode {mode}")
    wait_idle(a, b)
    marks = {i: boards[i].mark() for i in (a_id, b_id)}
    if mode == "direct":
        for other in others:
            boards[other].send("flood mute on")
    elif mode == "relay":
        a.send(f"relay block {b_id}")
        b.send(f"relay block {a_id}")
    time.sleep(1.0)
    rows = []
    try:
        connected = False
        for _ in range(3):
            if connect_over(TAG, "flood", a, b, a_id, b_id, " ", 8):
                connected = True
                break
            a.send("disconnect")
            time.sleep(3.0)
            wait_idle(a, b)
        if not connected:
            say("    could not connect")
            return [{"from": a_id, "to": b_id, "mode": mode,
                     "started": False, "connected": False}]
        time.sleep(2.5)  # A few frames, so the flood route is known.
        route = ui_line(a, r"connected \| [^|]+ \| box \w+ \| "
                           r"Route: ([^|]+?) \|")
        route = route.group(1) if route else "?"
        say(f"    route on board {a_id}'s screen: {route}")
        for _ in range(args.rounds):
            for sender, receiver in ((a_id, b_id), (b_id, a_id)):
                row = stream(boards[sender], boards[receiver], sender,
                             receiver, args.seconds)
                row["mode"] = mode
                row["route"] = route
                print_row(mode, row)
                rows.append(row)
        disconnect_over(TAG, a, b)
    finally:
        if mode == "direct":
            for other in others:
                boards[other].send("flood mute off")
            for other in others:  # Back before the next pair needs it.
                for end in (a_id, b_id):
                    wait_online(boards[end], other, marks[end], 25)
        elif mode == "relay":
            a.send(f"relay unblock {b_id}")
            b.send(f"relay unblock {a_id}")
            time.sleep(3.0)
    return rows


def totals(rows):
    done = [row for row in rows if row.get("started")]
    total = {key: sum(row[key] for row in done) for key in
             ("sent", "direct", "redundant", "retransmit", "late",
              "missing", "tx_send_errors")}
    total["streams"] = len(done)
    total["intact"] = sum(1 for row in done if row["intact"])
    total["not_started"] = len(rows) - len(done)
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("run")
    parser.add_argument("ports", nargs="+")
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--modes", default="all,direct,relay")
    args = parser.parse_args()
    if len(args.ports) < 2:
        sys.exit("give at least two ports")
    modes = [mode for mode in args.modes.split(",") if mode]
    if len(args.ports) < 3:  # Nothing to mute, nothing to relay through.
        modes = [mode for mode in modes if mode == "all"] or ["all"]

    EVIDENCE.mkdir(exist_ok=True)
    start = time.monotonic()
    opened = []
    for index, port in enumerate(args.ports):
        opened.append(Board(f"port{index}", port,
                            EVIDENCE / f"{args.run}_port{index}.txt", start))
    two_board_test.boards[:] = opened
    for board in opened:
        board.send("reboot")
        time.sleep(2)
    boards = {}
    for board, port in zip(opened, args.ports):
        board_id, channel = identify(board)
        if board_id is None:
            say(f"{port}: no boot banner; is the firmware flashed?")
            continue
        board.label = f"board{board_id}"
        boards[board_id] = board
        since = board.mark()
        board.send("routed")
        old = bool(board.wait(r"^ROUTED \| ", 2, since))
        say(f"{port}: board {board_id}, channel {channel}, "
            f"{'has' if old else 'NO'} routed relay command "
            f"({'the 6452b99-era build' if old else 'a newer build'})")
    if len(boards) < 2:
        for board in opened:
            board.close()
        sys.exit("fewer than two boards answered")

    ids = sorted(boards)
    say("\nWaiting for sessions between every pair...")
    ready = True
    for a_id, b_id in itertools.permutations(ids, 2):
        ok = bool(boards[a_id].wait(
            rf"{TAG} SESSION established with {b_id}\b", 45, 0)) and \
            wait_online(boards[a_id], b_id, 0, 30)
        if not ok:
            say(f"  board {a_id} has no session with board {b_id}")
            ready = False
    say("  all sessions up" if ready else "  carrying on without them")

    results = {}
    try:
        for a_id, b_id in itertools.combinations(ids, 2):
            for mode in modes:
                key = f"{a_id}-{b_id} {mode}"
                results[key] = run_pair(boards, a_id, b_id, mode, args)
        for board in boards.values():  # Final counters, for the logs.
            for command in ("flood", "report"):
                board.send(command)
                time.sleep(0.4)
        time.sleep(1.0)
    finally:
        for board in opened:
            board.close()
        for index, board in enumerate(opened):
            if board.label.startswith("board"):
                (EVIDENCE / f"{args.run}_port{index}.txt").replace(
                    EVIDENCE / f"{args.run}_{board.label}.txt")

    say(f"\nSummary ({args.seconds} s streams, {args.rounds} each way)")
    say(f"{'pair, mode':<14}{'streams':>8}{'sent':>7}{'first-time':>12}"
        f"{'redund.':>9}{'retrans.':>10}{'late':>6}{'MISSING':>9}"
        f"{'intact':>8}")
    summary = {}
    worst = 0
    for key, rows in results.items():
        total = totals(rows)
        summary[key] = total
        worst += total["missing"] + total["not_started"]
        sent = total["sent"] or 1
        say(f"{key:<14}{total['streams']:>8}{total['sent']:>7}"
            f"{100.0 * total['direct'] / sent:>11.1f}%"
            f"{total['redundant']:>9}{total['retransmit']:>10}"
            f"{total['late']:>6}{total['missing']:>9}"
            f"{total['intact']:>5}/{total['streams']}"
            f"{'  (' + str(total['not_started']) + ' not started)' if total['not_started'] else ''}")
    (EVIDENCE / f"{args.run}_summary.json").write_text(json.dumps(
        {"seconds": args.seconds, "rounds": args.rounds, "boards": ids,
         "summary": summary, "streams": results}, indent=2) + "\n")
    say(f"\nLogs and numbers: {EVIDENCE}/{args.run}_*")
    return 0 if worst == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
