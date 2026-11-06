"""What the two-board serial tests share (tools/mesh_test.py,
tools/relay_test.py): the PASS/FAIL record, the `ui` and voice checks, and
the start and end of main().
"""
import sys
import time

# The same serial helper as the LoRa edge tests (tools/lora_edge_test.py).
from lora_edge_test import EVIDENCE, Board

results = []
boards = []  # Board 1 and board 2, once open_boards() has opened them.


def check(ok, what):
    results.append((ok, what))
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)
    for board in boards:
        board.note(f"{'PASS' if ok else 'FAIL'} {what}")
    return ok


def ui_line(board, pattern, timeout=2.0):
    since = board.mark()
    board.send("ui")
    return board.wait(r"UI \| screen " + pattern, timeout, since)


def voice(sender, receiver, seconds, via):
    """Sends a test-signal stream and checks the receiver played it all."""
    s_tx, s_rx = sender.mark(), receiver.mark()
    sender.send(f"test {seconds}")
    started = sender.wait(rf"TX_START .* via={via}", 3, s_tx)
    heard = receiver.wait(rf"RX_START .* via={via}", 5, s_rx)
    ended = receiver.wait(r"RX_END .* intact=(\w+)", seconds + 10, s_rx)
    sender.wait(r"TX_END", 5, s_tx)
    time.sleep(2.0)  # The sender lingers 1.5 s answering NACKs.
    return bool(started and heard and ended and ended.group(1) == "yes")


def open_boards(doc, tests, default):
    """Reads `RUN PORT1 PORT2 [TEST ...]` (exits with `doc` if short),
    opens both boards and reboots them, so each log starts at boot (opening
    a port doesn't reset the board on every OS), board 1 first, as at
    power-on. Returns board 1, board 2 and the tests to run."""
    if len(sys.argv) < 4:
        sys.exit(doc)
    run, port1, port2 = sys.argv[1:4]
    names = sys.argv[4:] or default
    unknown = [name for name in names if name not in tests]
    if unknown:
        sys.exit(f"unknown test {', '.join(unknown)}; choose from "
                 f"{' '.join(tests)}")
    EVIDENCE.mkdir(exist_ok=True)
    start = time.monotonic()
    b1 = Board("board1", port1, EVIDENCE / f"{run}_board1.txt", start)
    b2 = Board("board2", port2, EVIDENCE / f"{run}_board2.txt", start)
    boards[:] = [b1, b2]
    b1.send("reboot")
    time.sleep(2)
    b2.send("reboot")
    return b1, b2, names


def finish():
    """Prints the tally and closes both boards; returns the exit code."""
    passed = sum(1 for ok, _ in results if ok)
    print(f"{passed}/{len(results)} checks passed")
    for board in boards:
        board.close()
    return 0 if results and passed == len(results) else 1
