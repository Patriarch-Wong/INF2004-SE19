"""Flood mesh and routed relay tests for boards 1 and 2, driven over
serial, reacting to what each board logs. ESP-NOW only: no router, no root.

The script reboots both boards over serial first (`reboot`), board 1 two
seconds before board 2. Logs go to evidence/<run>_board1.txt and
_board2.txt; tools/check_streams.py can compare their voice streams
afterwards.

    python tools/relay_test.py relay1 COM11 COM4 [TEST ...]

Port 1 must be board 1. TEST names are the keys of TESTS below (default:
all but `middle`). `middle` needs board 3 powered on with this build (no
serial needed): boards 1 and 2 block each other's relayed frames
(`relay block`), so everything must go through board 3.
"""
import sys
import time

import two_board_test
from two_board_test import (check, connect_over, disconnect_over, finish,
                            open_boards, ui_line, voice)

B1 = B2 = None

WAYS = {
    # way: (log tag, serial word, menu label in `ui`)
    "flood": ("FLOOD", "flood", "Flood mesh"),
    "routed": ("ROUTED", "routed", "Routed relay"),
}


def wait_idle(*boards):
    two_board_test.wait_idle(
        boards, "menu|espnow|mesh|flood|routed|lora|volume|status")


def connect(way, a, b, a_id, b_id):
    """a asks b over `way` with serial commands; b accepts."""
    tag, word, _ = WAYS[way]
    return connect_over(tag, word, a, b, a_id, b_id, " ", 3)


def disconnect(way, a, b):
    return disconnect_over(WAYS[way][0], a, b)


def route_on_screen(board, way):
    """The route line of the connected screen, or ''."""
    label = WAYS[way][2]
    match = ui_line(board, rf"connected \| {label} \| box \w+ \| "
                           r"Route: ([^|]+?) \|")
    return match.group(1) if match else ""


def test_sessions():
    print("R1 sessions and presence over both ways", flush=True)
    for way in WAYS:
        tag = WAYS[way][0]
        check(bool(B1.wait(rf"{tag} SESSION established with 2", 30, 0) and
                   B2.wait(rf"{tag} SESSION established with 1", 10, 0)),
              f"{way}: session made on both boards")
        check(bool(B1.wait(rf"{tag} PEER 2 \(Board 2\) online", 20, 0) and
                   B2.wait(rf"{tag} PEER 1 \(Board 1\) online", 20, 0)),
              f"{way}: each board sees the other online")
    check(bool(B1.wait(r"ROUTED route to 2 \(Board 2\): ", 20, 0)),
          "routed: board 1 found a route to board 2")


def run_way(way):
    tag = WAYS[way][0]
    wait_idle(B1, B2)
    if not check(connect(way, B1, B2, 1, 2), f"{way}: board 1 connects to "
                                              f"board 2, board 2 accepts"):
        return
    route = route_on_screen(B1, way)
    check(route.startswith("1 > 2"), f"{way}: route on board 1's screen "
                                     f"({route or 'none'})")
    since = B2.mark()
    B1.send("hello")
    check(bool(B2.wait(rf'{tag} TEXT from 1 \(Board 1\): "Hello from '
                       r'Board 1"', 5, since)), f"{way}: hello 1 -> 2")
    time.sleep(1.2)
    since = B1.mark()
    B2.send("hello")
    check(bool(B1.wait(rf'{tag} TEXT from 2 \(Board 2\): "Hello from '
                       r'Board 2"', 5, since)), f"{way}: hello 2 -> 1")
    check(voice(B1, B2, 5, way), f"{way}: 5 s voice 1 -> 2 intact")
    check(voice(B2, B1, 5, way), f"{way}: 5 s voice 2 -> 1 intact")
    check(disconnect(way, B2, B1), f"{way}: disconnect drops both")


def test_flood():
    print("R2 flood mesh: connect, hello, voice both ways, disconnect",
          flush=True)
    run_way("flood")


def test_routed():
    print("R3 routed relay: connect, hello, voice both ways, disconnect",
          flush=True)
    run_way("routed")


def test_busy():
    print("R4 one connection at a time", flush=True)
    wait_idle(B1, B2)
    if not check(connect("flood", B1, B2, 1, 2), "flood connected"):
        return
    since = B1.mark()
    B1.send("routed connect 2")
    check(bool(B1.wait(r"ROUTED LINK refused: a connection over another "
                       r"radio", 3, since)), "routed connect refused")
    since = B1.mark()
    B1.send("connect 2")
    check(bool(B1.wait(r"^LINK refused: a flood mesh or routed relay", 3,
                       since)), "ESP-NOW connect refused")
    since = B1.mark()
    B1.send("mesh connect 2")
    check(bool(B1.wait(r"MESH LINK refused: (a flood mesh|not in the mesh)",
                       3, since)), "ESP-MESH connect refused")
    disconnect("flood", B1, B2)


def test_loss():
    print("R5 link loss: board 2 goes silent on each way", flush=True)
    for way in WAYS:
        tag, word, _ = WAYS[way]
        wait_idle(B1, B2)
        if not check(connect(way, B1, B2, 1, 2), f"{way}: connected"):
            continue
        s1, s2 = B1.mark(), B2.mark()
        B2.send(f"{word} mute on")
        check(bool(B1.wait(rf"{tag} LINK disconnected", 15, s1) and
                   B2.wait(rf"{tag} LINK disconnected", 15, s2)),
              f"{way}: both drop the connection within 15 s")
        since = B1.mark()
        B2.send(f"{word} mute off")
        check(bool(B1.wait(rf"{tag} PEER 2 \(Board 2\) online", 20, since)),
              f"{way}: board 2 back online after unmuting")


def test_middle():
    print("R6 board 3 as the middleman (boards 1 and 2 block each other)",
          flush=True)
    wait_idle(B1, B2)
    s1 = B1.mark()
    B1.send("relay block 2")
    B2.send("relay block 1")
    try:
        check(bool(B1.wait(r"ROUTED route to 2 \(Board 2\): 1 > 3 > 2, via "
                           r"Board 3", 30, s1)),
              "routed: board 1 finds the route through board 3")
        for way in WAYS:
            wait_idle(B1, B2)
            if not check(connect(way, B1, B2, 1, 2),
                         f"{way}: connects through board 3"):
                continue
            time.sleep(2.5)  # A few frames, so the flood route is known.
            route = route_on_screen(B1, way)
            check("via Board 3" in route,
                  f"{way}: board 1's screen shows the route ({route})")
            check(voice(B1, B2, 5, way), f"{way}: 5 s voice 1 -> 3 -> 2 "
                                         f"intact")
            check(voice(B2, B1, 5, way), f"{way}: 5 s voice 2 -> 3 -> 1 "
                                         f"intact")
            disconnect(way, B1, B2)
    finally:
        B1.send("relay unblock 2")
        B2.send("relay unblock 1")


TESTS = {
    "sessions": test_sessions,
    "flood": test_flood,
    "routed": test_routed,
    "busy": test_busy,
    "loss": test_loss,
    "middle": test_middle,
}
DEFAULT = ["sessions", "flood", "routed", "busy", "loss"]


def main():
    global B1, B2
    B1, B2, names = open_boards(__doc__, TESTS, DEFAULT)
    try:
        if names[0] != "sessions":
            for tag in ("FLOOD", "ROUTED"):
                B1.wait(rf"{tag} PEER 2 \(Board 2\) online", 60, 0)
        for name in names:
            TESTS[name]()
    finally:
        code = finish()
    return code


if __name__ == "__main__":
    sys.exit(main())
