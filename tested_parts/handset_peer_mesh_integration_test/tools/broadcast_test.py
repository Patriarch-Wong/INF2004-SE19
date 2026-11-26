"""Broadcast tests for boards 1 and 2, driven over serial, reacting to what
each board logs: ESP-NOW broadcast voice and hello, and LoRa broadcast
texts, to every board at once, with no connection.

The script reboots both boards over serial first (`reboot`), board 1 two
seconds before board 2. Logs go to evidence/<run>_board1.txt and
_board2.txt; tools/check_streams.py can compare their voice streams
afterwards.

    python tools/broadcast_test.py bcast1 COM11 COM4 [TEST ...]

Port 1 must be board 1. TEST names are the keys of TESTS below (default:
all, in that order). A third board (board 3) may be on or off: if it is
on, it hears the broadcasts too, which you can see on its screen.
"""
import sys
import time

from two_board_test import check, finish, open_boards, ui_line, voice

B1 = B2 = None


def keys_ready(board, peer, lora, timeout):
    """`board` logged that `peer` took its broadcast key since boot."""
    tag = "LORA BCAST" if lora else "BCAST"
    return bool(board.wait(rf"^{tag} key delivered to {peer} ", timeout, 0))


def to_menu_row(board, label):
    """Brings `board` to the top menu and opens the row `label`."""
    for _ in range(16):
        screen, selected = board.ui()
        if screen == "menu":
            if selected == label:
                board.press("ok", 0.6)
                return True
            board.press("down")
        elif screen == "disconnected":
            time.sleep(1.0)
        else:
            board.press("cancel", 0.6)
    return False


def test_keys():
    print("B1 broadcast keys reach the other board over each radio",
          flush=True)
    check(keys_ready(B1, 2, False, 30), "board 2 took board 1's ESP-NOW key")
    check(keys_ready(B2, 1, False, 30), "board 1 took board 2's ESP-NOW key")
    check(keys_ready(B1, 2, True, 90), "board 2 took board 1's LoRa key")
    check(keys_ready(B2, 1, True, 30), "board 1 took board 2's LoRa key")


def test_hello():
    print("B2 hello to all over ESP-NOW", flush=True)
    s1, s2 = B1.mark(), B2.mark()
    B1.send("bcast hello")
    check(bool(B1.wait(r"BCAST TEXT sent to all: .* key: .*\b2\b", 3, s1)),
          "board 1 sent it (board 2 has the key)")
    check(bool(B2.wait(r'BCAST TEXT from 1 \(Board 1\): "Hello all from '
                       r'Board 1"', 3, s2)),
          "board 2 got it")
    check(bool(ui_line(B2, r'.* banner "Hello all from Board 1"')),
          "board 2 shows it in the banner")
    time.sleep(1.2)  # One hello a second.


def test_voice():
    print("B3 push-to-talk to all over ESP-NOW (test signal)", flush=True)
    s2 = B2.mark()
    B1.send("bcast test 5")
    check(voice(B1, B2, 5, "broadcast"), "board 2 played the whole stream")
    check(bool(B2.find(r"RX_START .* from=1 ", s2)), "it came from board 1")
    check(bool(ui_line(B2, r"broadcast \|")),
          "board 2 jumped to the Broadcast page")


def test_reply():
    print("B4 the other way: board 2 talks to all", flush=True)
    B2.send("bcast test 3")
    check(voice(B2, B1, 3, "broadcast"), "board 1 played the whole stream")


def test_dpad():
    print("B5 the same from the D-pad (serial press)", flush=True)
    if not check(to_menu_row(B1, "Broadcast") and
                 bool(ui_line(B1, r"broadcast \|")),
                 "board 1 on the Broadcast page"):
        return
    since = B2.mark()
    B1.press("right")
    check(bool(B2.wait(r"BCAST TEXT from 1", 5, since)),
          "RIGHT says hello to all")
    time.sleep(1.2)
    since = B2.mark()
    B1.send("press ok 3000")  # Hold OK 3 s: push-to-talk to all.
    check(bool(B2.wait(r"RX_START .* via=broadcast", 5, since)),
          "holding OK talks to all")
    check(bool(B2.wait(r"RX_END .* intact=yes", 12, since)),
          "board 2 played the whole stream")
    time.sleep(2.0)
    B1.press("cancel", 0.6)


def test_lora():
    print("B6 hello and bye to all over LoRa", flush=True)
    s1, s2 = B1.mark(), B2.mark()
    B1.send("lora bcast hello")
    check(bool(B1.wait(r"LORA BCAST TEXT sent to all", 3, s1)),
          "board 1 sent hello to all")
    check(bool(B2.wait(r'LORA BCAST TEXT from 1 \(Board 1\): "Hello all '
                       r'from Board 1"', 5, s2)),
          "board 2 got it over LoRa")
    time.sleep(1.5)
    s1 = B1.mark()
    B2.send("lora bcast bye")
    check(bool(B1.wait(r'LORA BCAST TEXT from 2 \(Board 2\): "Bye all from '
                       r'Board 2"', 5, s1)),
          "board 1 got bye from board 2")
    time.sleep(1.2)


def test_busy():
    print("B7 no broadcast from a connection", flush=True)
    s1, s2 = B1.mark(), B2.mark()
    B1.send("connect 2")  # ESP-NOW peer to peer.
    if not check(bool(B2.wait(r"^LINK request \| 1 ", 8, s2)),
                 "board 2 asked over ESP-NOW"):
        return
    B2.send("accept")
    if not check(bool(B1.wait(r"^LINK connected", 8, s1)),
                 "boards 1 and 2 connected over ESP-NOW"):
        return
    since = B1.mark()
    B1.send("bcast hello")
    check(bool(B1.wait(r"BCAST TEXT refused: a connection is in progress",
                       3, since)), "hello to all refused")
    B1.send("bcast test 2")
    check(bool(B1.wait(r"BCAST TALK refused: a connection is in progress",
                       3, since)), "talk to all refused")
    s1, s2 = B1.mark(), B2.mark()
    B1.send("disconnect")
    check(bool(B1.wait(r"^LINK disconnected", 8, s1) and
               B2.wait(r"^LINK disconnected", 8, s2)), "disconnected")
    time.sleep(1.5)


def test_reboot():
    print("B8 board 2 reboots: board 1's key goes to it again", flush=True)
    s1 = B1.mark()
    B2.send("reboot")
    check(bool(B1.wait(r"^BCAST key delivered to 2 ", 40, s1)),
          "board 2 took board 1's ESP-NOW key again")
    time.sleep(1.5)
    s2 = B2.mark()
    B1.send("bcast hello")
    check(bool(B2.wait(r"BCAST TEXT from 1", 3, s2)),
          "board 2 hears board 1's broadcasts again")


TESTS = {
    "keys": test_keys,
    "hello": test_hello,
    "voice": test_voice,
    "reply": test_reply,
    "dpad": test_dpad,
    "lora": test_lora,
    "busy": test_busy,
    "reboot": test_reboot,
}


def main():
    global B1, B2
    B1, B2, names = open_boards(__doc__, TESTS, list(TESTS))
    try:
        if names[0] != "keys":
            keys_ready(B1, 2, False, 30)
            keys_ready(B1, 2, True, 90)
        for name in names:
            TESTS[name]()
    finally:
        code = finish()
    return code


if __name__ == "__main__":
    sys.exit(main())
