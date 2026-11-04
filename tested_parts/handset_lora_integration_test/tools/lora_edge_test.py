"""LoRa edge-case tests for two boards, driven over serial with the serial
D-pad (press NAME), reacting to what each board logs.

Opening the ports resets both boards. Each test starts from the boards'
menus or lists, drives them with button presses, and checks the log lines
the firmware prints (LORA LINK ..., UI | screen ...). Logs go to
evidence/<run>_board1.txt and _board2.txt.

    ~/.platformio/penv/bin/python tools/lora_edge_test.py edge1 \\
        /dev/cu.usbmodem5B5F0216001 /dev/cu.usbmodem5B5F0215971 [TEST ...]
"""
import re
import sys
import threading
import time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run with ~/.platformio/penv/bin/python")

EVIDENCE = Path(__file__).resolve().parent.parent / "evidence"
BAUD = 921600


class Board:
    def __init__(self, label, port, log_path, start):
        self.label = label
        self.start = start
        self.lines = []  # (seconds, text)
        self.lock = threading.Lock()
        self.link = serial.Serial()
        self.link.port = port
        self.link.baudrate = BAUD
        self.link.timeout = 0.05
        self.link.dtr = False
        self.link.rts = False
        self.link.open()
        self.log = log_path.open("w")
        self.running = True
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def now(self):
        return time.monotonic() - self.start

    def read(self):
        pending = b""
        while self.running:
            try:
                pending += self.link.read(1024)
            except (OSError, serial.SerialException):
                break
            *chunks, pending = pending.split(b"\n")
            for chunk in chunks:
                text = chunk.decode("utf-8", "replace").rstrip("\r")
                stamp = self.now()
                with self.lock:
                    self.lines.append((stamp, text))
                self.log.write(f"{stamp:7.2f} {text}\n")
            self.log.flush()

    def mark(self):
        with self.lock:
            return len(self.lines)

    def send(self, text):
        self.log.write(f"{self.now():7.2f} >>> {text}\n")
        self.link.write(text.encode() + b"\n")

    def note(self, text):
        self.log.write(f"{self.now():7.2f} ### {text}\n")

    def find(self, pattern, since=0):
        regex = re.compile(pattern)
        with self.lock:
            for i in range(since, len(self.lines)):
                match = regex.search(self.lines[i][1])
                if match:
                    return i, match
        return None, None

    def wait(self, pattern, timeout, since=None):
        since = self.mark() if since is None else since
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            index, match = self.find(pattern, since)
            if match:
                return match
            time.sleep(0.02)
        return None

    def ui(self):
        since = self.mark()
        self.send("ui")
        match = self.wait(r'UI \| screen (\S+)(?: \| selected (\d+)/(\d+) '
                          r'"([^"]*)")?', 2.0, since)
        if not match:
            return None, None
        return match.group(1), match.group(4)

    def press(self, name, settle=0.35):
        self.send(f"press {name}")
        time.sleep(settle)

    def close(self):
        self.running = False
        self.thread.join(timeout=1)
        self.link.close()
        self.log.close()


results = []


def check(ok, what):
    results.append((ok, what))
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)
    for board in BOARDS:
        board.note(f"{'PASS' if ok else 'FAIL'} {what}")
    return ok


def to_list(board, kind, peer):
    """Brings `board` to the ESP-NOW or LoRa list with Board `peer`
    selected, from wherever it is."""
    want_screen = "lora" if kind == "lora" else "espnow"
    want_menu = "LoRa" if kind == "lora" else "ESP-NOW"
    want_row = f"Board {peer}" if kind == "lora" else f"Connect Board {peer}"
    for _ in range(14):
        screen, label = board.ui()
        if screen == "disconnected":
            time.sleep(1.0)
        elif screen == want_screen:
            if label == want_row:
                return True
            board.press("down")
        elif screen == "menu":
            if label == want_menu:
                board.press("ok", 0.6)
            else:
                board.press("down")
        else:
            board.press("cancel", 0.6)
    return False


def lora_connect(a, b):
    """a asks b over LoRa from the list; b accepts after the guard."""
    if not (to_list(a, "lora", 2 if a is B1 else 1)):
        return False
    since_b = b.mark()
    since_a = a.mark()
    a.press("ok", 0.1)
    if not b.wait(r"LORA LINK request", 8, since_b):
        return False
    time.sleep(0.8)
    b.press("ok", 0.1)
    return bool(a.wait(r"LORA LINK connected", 8, since_a) and
                b.wait(r"LORA LINK connected", 1, since_b))


def wait_idle(*boards):
    for board in boards:
        for _ in range(10):
            screen, _ = board.ui()
            if screen in ("menu", "lora", "espnow"):
                break
            time.sleep(0.7)


def test_session():
    print("E1 boot: LoRa session", flush=True)
    m1 = B1.wait(r"LORA SESSION established with 2", 30, 0)
    m2 = B2.wait(r"LORA SESSION established with 1", 5, 0)
    check(bool(m1 and m2), "LoRa session made on both boards")
    _, ok1 = B1.find(r"LORA ok")
    i1, _ = B1.find(r"LORA SESSION established with 2")
    i2, _ = B2.find(r"LORA ok")
    if i1 is not None and i2 is not None:
        t_ok = B2.lines[i2][0]
        t_session = B1.lines[i1][0]
        check(t_session - t_ok < 6,
              f"session {t_session - t_ok:.1f} s after the later radio "
              f"started (< 6 s)")


def test_decline():
    print("E4a decline", flush=True)
    to_list(B1, "lora", 2)
    s1, s2 = B1.mark(), B2.mark()
    B1.press("ok", 0.1)
    check(bool(B2.wait(r"LORA LINK request \| 1 .* over LoRa", 8, s2)),
          "board 2 shows the LoRa request")
    screen, _ = B2.ui()
    check(screen == "request", f"board 2 on the request screen ({screen})")
    time.sleep(0.8)
    B2.press("cancel", 0.1)
    check(bool(B1.wait(r"LORA LINK disconnected from 2 .*declined by peer",
                       8, s1)), "board 1: declined by peer")
    check(bool(B2.wait(r"LORA LINK disconnected from 1 .*declined here", 2,
                       s2)), "board 2: declined here")
    wait_idle(B1, B2)


def test_cancel_asking():
    print("E4b cancel while asking", flush=True)
    to_list(B1, "lora", 2)
    s1, s2 = B1.mark(), B2.mark()
    B1.press("ok", 0.1)
    B2.wait(r"LORA LINK request", 8, s2)
    time.sleep(1.0)
    B1.press("cancel", 0.1)
    check(bool(B1.wait(r"LORA LINK disconnected from 2 .*cancelled here", 3,
                       s1)), "board 1: cancelled here")
    check(bool(B2.wait(r"LORA LINK disconnected from 1 .*peer stopped asking",
                       10, s2)), "board 2: request withdrawn")
    wait_idle(B1, B2)


def test_guard():
    print("E8 request guard (OK within 500 ms is ignored)", flush=True)
    to_list(B1, "lora", 2)
    s1, s2 = B1.mark(), B2.mark()
    B1.press("ok", 0.0)
    if not B2.wait(r"UI screen=request", 8, s2):
        check(False, "board 2 shows the request")
        return
    B2.press("ok", 0.0)
    time.sleep(1.0)
    check(not B2.find(r"LORA LINK connected", s2)[1],
          "OK pressed right after the request appeared did nothing")
    B2.press("ok", 0.1)
    check(bool(B1.wait(r"LORA LINK connected", 8, s1)),
          "a later OK accepts")
    B1.press("cancel", 0.1)
    check(bool(B2.wait(r"LORA LINK disconnected from 1 .*disconnected by "
                       r"peer", 8, s2)), "board 1 disconnects, board 2 told")
    wait_idle(B1, B2)


def test_no_answer():
    print("E4c no answer (30 s)", flush=True)
    to_list(B1, "lora", 2)
    s1, s2 = B1.mark(), B2.mark()
    B1.press("ok", 0.1)
    B2.wait(r"LORA LINK request", 8, s2)
    m = B1.wait(r"LORA LINK disconnected from 2 .*no answer", 36, s1)
    check(bool(m), "board 1 gives up: no answer")
    check(bool(B2.wait(r"LORA LINK disconnected from 1", 12, s2)),
          "board 2's request screen ends too")
    wait_idle(B1, B2)


def test_glare():
    print("E7 both ask at once", flush=True)
    to_list(B1, "lora", 2)
    to_list(B2, "lora", 1)
    s1, s2 = B1.mark(), B2.mark()
    B1.send("press ok")
    B2.send("press ok")
    time.sleep(12)
    c1 = B1.find(r"LORA LINK connected to 2", s1)[1]
    c2 = B2.find(r"LORA LINK connected to 1", s2)[1]
    screen1, _ = B1.ui()
    screen2, _ = B2.ui()
    consistent = (screen1 == "lora_connected") == (screen2 ==
                                                   "lora_connected")
    check(consistent, f"both boards agree after 12 s ({screen1} / "
                      f"{screen2}, connected logs {bool(c1)}/{bool(c2)})")
    for board in (B1, B2):
        screen, _ = board.ui()
        if screen in ("lora_connected", "connecting", "request"):
            board.press("cancel", 0.3)
    time.sleep(1.5)
    wait_idle(B1, B2)


def test_busy():
    print("E6 one connection at a time", flush=True)
    if not check(lora_connect(B1, B2), "LoRa connected 1-2"):
        return
    s1 = B1.mark()
    B1.send("connect 2")
    check(bool(B1.wait(r"LINK refused: a LoRa connection is in progress",
                       2, s1)), "ESP-NOW connect refused during LoRa")
    B1.press("cancel", 0.3)
    B2.wait(r"LORA LINK disconnected", 8)
    wait_idle(B1, B2)
    # ESP-NOW connection, then LoRa refused.
    to_list(B1, "espnow", 2)
    s1, s2 = B1.mark(), B2.mark()
    B1.press("ok", 0.1)
    if not B2.wait(r"LINK request \| 1", 6, s2):
        check(False, "ESP-NOW request reached board 2")
        return
    time.sleep(0.8)
    B2.press("ok", 0.1)
    check(bool(B1.wait(r"^\s*LINK connected to 2", 6, s1)),
          "ESP-NOW connected 1-2")
    s1 = B1.mark()
    B1.send("lora connect 2")
    check(bool(B1.wait(r"LORA LINK refused: an ESP-NOW connection", 2, s1)),
          "LoRa connect refused during ESP-NOW")
    s2 = B2.mark()
    B2.send("lora connect 1")
    check(bool(B2.wait(r"LORA LINK refused: an ESP-NOW connection", 2, s2)),
          "and on board 2")
    B1.press("cancel", 0.3)
    B2.wait(r"LINK disconnected", 6)
    wait_idle(B1, B2)


def test_texts_outside():
    print("E10 texts only inside a connection", flush=True)
    wait_idle(B1, B2)
    s1 = B1.mark()
    B1.send("lora hello")
    time.sleep(0.5)
    check(not B1.find(r"LORA TEXT sent", s1)[1] and
          not B1.find(r"LORA TX -> 2 \| type 1", s1)[1],
          "lora hello while not connected sends nothing")
    to_list(B1, "lora", 2)
    s1 = B1.mark()
    B1.press("up")
    B1.press("down")
    check(not B1.find(r"LORA TX -> 2 \| type 1", s1)[1],
          "UP/DOWN on the LoRa list only move the selection")


def test_link_loss():
    print("E9a link loss: board 2 out of range (mute)", flush=True)
    if not check(lora_connect(B1, B2), "LoRa connected 1-2"):
        return
    s1, s2 = B1.mark(), B2.mark()
    t0 = time.monotonic()
    B2.send("lora mute on")
    m1 = B1.wait(r"LORA LINK disconnected from 2 .*link lost", 30, s1)
    t1 = time.monotonic() - t0
    m2 = B2.wait(r"LORA LINK disconnected from 1 .*link lost", 8, s2)
    check(bool(m1), f"board 1: link lost after {t1:.1f} s (20-27 s)")
    check(bool(m1) and 19 <= t1 <= 28, "within the 20 s timeout (+ ping)")
    check(bool(m2), "board 2: link lost too")
    B2.send("lora mute off")
    time.sleep(1)
    wait_idle(B1, B2)


def test_reboot():
    print("E9b reboot while connected", flush=True)
    if not check(lora_connect(B1, B2), "LoRa connected 1-2"):
        return
    s1 = B1.mark()
    t0 = time.monotonic()
    B2.send("reboot")
    m = B1.wait(r"LORA LINK (disconnected from 2|2 \(Board 2\) made a new)",
                30, s1)
    t = time.monotonic() - t0
    check(bool(m), f"board 1 ends the connection after {t:.1f} s "
                   f"({m.group(0)[:60] if m else 'none'})")
    check(bool(B1.wait(r"LORA SESSION established with 2", 20, s1)),
          "LoRa session made again after the reboot")
    wait_idle(B1, B2)


def test_reconnect_texts():
    print("E2 reconnect and texts after everything above", flush=True)
    if not check(lora_connect(B2, B1), "board 2 asks, board 1 accepts"):
        return
    s1 = B1.mark()
    B2.press("up")
    check(bool(B1.wait(r'LORA TEXT from 2 .*"Hello from Board 2"', 5, s1)),
          "Hello from Board 2 arrives")
    s2 = B2.mark()
    time.sleep(1.1)
    B1.press("down")
    check(bool(B2.wait(r'LORA TEXT from 1 .*"Bye from Board 1"', 5, s2)),
          "Bye from Board 1 arrives")
    B2.press("cancel", 0.3)
    check(bool(B1.wait(r"LORA LINK disconnected from 2", 8, s1)),
          "disconnect from board 2")
    wait_idle(B1, B2)


# ESP-NOW, and ESP-NOW with LoRa together.

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_streams  # noqa: E402


def records_since(board, tag, since):
    found = {}
    with board.lock:
        lines = [text for _, text in board.lines[since:]]
    for line in lines:
        at = line.find(tag + " ")
        if at < 0:
            continue
        fields = dict(check_streams.FIELD.findall(line[at + len(tag):]))
        if "stream" in fields:
            found[fields["stream"].rstrip(":")] = fields
    return found


def voice_ok(sender, receiver, s_tx, s_rx, what):
    """Every stream `sender` ended since s_tx played intact on `receiver`."""
    result = check_streams.check(records_since(sender, "TX_END", s_tx),
                                 records_since(receiver, "RX_END", s_rx),
                                 set(records_since(sender, "TX_YIELD", s_tx)))
    detail = ", ".join(
        f"{x['frames_sent']} frames {'intact' if x['passed'] else x['reason']}"
        for x in result["streams"]) or "no stream"
    return check(result["passed"], f"{what}: {detail}")


def send_voice(sender, receiver, seconds, what):
    """`test S` on sender; waits for its TX_END and the receiver's RX_END
    for the same stream. Returns the RX_END fields (or None)."""
    s_tx, s_rx = sender.mark(), receiver.mark()
    sender.send(f"test {seconds}")
    start = sender.wait(r"TX_START stream=(\w+)", 3, s_tx)
    if not start:
        check(False, f"{what}: no TX_START")
        return None
    stream = start.group(1)
    sender.wait(rf"TX_END stream={stream}", seconds + 8, s_tx)
    receiver.wait(rf"RX_END stream={stream}", 4, s_rx)
    voice_ok(sender, receiver, s_tx, s_rx, what)
    return records_since(receiver, "RX_END", s_rx).get(stream)


def esp_connect(a, b):
    peer = 2 if a is B1 else 1
    if not to_list(a, "espnow", peer):
        return False
    s_a, s_b = a.mark(), b.mark()
    a.press("ok", 0.1)
    if not b.wait(r"^LINK request \| ", 6, s_b):
        return False
    time.sleep(0.8)
    b.press("ok", 0.1)
    return bool(a.wait(r"^LINK connected to", 6, s_a) and
                b.wait(r"^LINK connected to", 2, s_b))


def test_esp_basic():
    print("X1 ESP-NOW in this build: connect, hello, voice both ways",
          flush=True)
    if not check(esp_connect(B1, B2), "ESP-NOW connected 1-2 (D-pad)"):
        return
    s2 = B2.mark()
    B1.press("right")
    check(bool(B2.wait(r'Hello from Board 1', 3, s2)),
          "RIGHT on board 1: Hello arrives over ESP-NOW")
    check(not B1.find(r"LORA TX -> 2 \| type 1", s2)[1],
          "nothing sent over LoRa for it")
    time.sleep(1.2)
    send_voice(B1, B2, 5, "voice 1->2")
    send_voice(B2, B1, 5, "voice 2->1")
    s1 = B1.mark()
    B2.press("cancel", 0.3)
    check(bool(B1.wait(r"^LINK disconnected from 2 .*disconnected by peer",
                       4, s1)), "disconnect over ESP-NOW")
    wait_idle(B1, B2)


def test_esp_edges():
    print("X2 ESP-NOW decline, cancel, guard, PTT collision", flush=True)
    to_list(B1, "espnow", 2)
    s1, s2 = B1.mark(), B2.mark()
    B1.press("ok", 0.1)
    B2.wait(r"^LINK request", 6, s2)
    time.sleep(0.8)
    B2.press("cancel", 0.1)
    check(bool(B1.wait(r"^LINK disconnected from 2 .*declined by peer", 4,
                       s1)), "ESP-NOW decline")
    wait_idle(B1, B2)
    to_list(B1, "espnow", 2)
    s1, s2 = B1.mark(), B2.mark()
    B1.press("ok", 0.0)
    B2.wait(r"UI screen=request", 6, s2)
    B2.press("ok", 0.0)
    time.sleep(0.8)
    check(not B2.find(r"^LINK connected", s2)[1],
          "ESP-NOW request guard: early OK ignored")
    B1.press("cancel", 0.1)
    check(bool(B2.wait(r"^LINK disconnected from 1 .*peer stopped asking",
                       6, s2)), "ESP-NOW cancel while asking")
    wait_idle(B1, B2)
    if not check(esp_connect(B1, B2), "ESP-NOW connected for PTT collision"):
        return
    time.sleep(1.5)  # The accept's serial press must be released first.
    s1, s2 = B1.mark(), B2.mark()
    B1.send("hold ok")
    B2.send("hold ok")
    time.sleep(3)
    B1.send("release ok")
    B2.send("release ok")
    time.sleep(3)
    yielded = (B1.find(r"TX_YIELD", s1)[1] or B2.find(r"TX_YIELD", s2)[1])
    check(bool(yielded), "both press talk: one board yields (TX_YIELD)")
    winner_is_1 = bool(B2.find(r"TX_YIELD", s2)[1])
    if winner_is_1:
        voice_ok(B1, B2, s1, s2, "winner board 1 heard")
    else:
        voice_ok(B2, B1, s2, s1, "winner board 2 heard")
    B1.press("cancel", 0.3)
    B2.wait(r"^LINK disconnected", 4)
    wait_idle(B1, B2)


def test_switch():
    print("X3 switch radios back to back", flush=True)
    for kind in ("espnow", "lora", "espnow", "lora"):
        if kind == "espnow":
            ok = esp_connect(B1, B2)
        else:
            ok = lora_connect(B1, B2)
        if not check(ok, f"{kind} connected"):
            return
        s1 = B1.mark()
        B2.press("cancel", 0.2)
        pattern = (r"^LINK disconnected from 2" if kind == "espnow" else
                   r"LORA LINK disconnected from 2")
        check(bool(B1.wait(pattern, 8, s1)), f"{kind} disconnected")
        wait_idle(B1, B2)


def test_cross_glare():
    print("X4 cross-radio glare: 1 asks over ESP-NOW, 2 over LoRa at once",
          flush=True)
    to_list(B1, "espnow", 2)
    to_list(B2, "lora", 1)
    s1, s2 = B1.mark(), B2.mark()
    B1.send("press ok")
    B2.send("press ok")
    time.sleep(10)
    screen1, _ = B1.ui()
    screen2, _ = B2.ui()
    stuck = {"connecting", "request"}
    ends1 = [t for _, t in B1.lines[s1:] if "LINK disconnected" in t]
    ends2 = [t for _, t in B2.lines[s2:] if "LINK disconnected" in t]
    check(not (screen1 in stuck and screen2 in stuck) or
          (screen1, screen2) in (("connecting", "request"),
                                 ("request", "connecting")),
          f"no deadlock after 10 s ({screen1} / {screen2}; "
          f"board 1 ends {[e[:70] for e in ends1]}, board 2 ends "
          f"{[e[:70] for e in ends2]})")
    for board in (B1, B2):
        screen, _ = board.ui()
        if screen in ("connected", "lora_connected", "connecting",
                      "request"):
            board.press("cancel", 0.3)
    time.sleep(1.5)
    wait_idle(B1, B2)


def test_lora_out_of_espnow():
    print("X5 ESP-NOW out of range (board 2 on channel 1), LoRa still "
          "connects", flush=True)
    s1 = B1.mark()
    B2.send("radio 1 1")
    check(bool(B1.wait(r"PEER 2 \(Board 2\) offline", 12, s1)),
          "board 1 sees board 2 offline on ESP-NOW")
    s1 = B1.mark()
    B1.send("connect 2")
    check(bool(B1.wait(r"LINK refused: 2 is offline", 2, s1)),
          "ESP-NOW connect refused: offline")
    if not check(lora_connect(B1, B2), "LoRa connects while ESP-NOW is out"):
        B2.send("radio 11 1")
        return
    s2 = B2.mark()
    B1.press("up")
    check(bool(B2.wait(r'LORA TEXT from 1 .*"Hello from Board 1"', 5, s2)),
          "LoRa Hello arrives")
    s1 = B1.mark()
    B2.send("radio 11 1")
    check(bool(B1.wait(r"PEER 2 \(Board 2\) online", 12, s1)),
          "ESP-NOW back: board 2 online again")
    screen, _ = B1.ui()
    check(screen == "lora_connected",
          f"LoRa connection unaffected ({screen})")
    s1 = B1.mark()
    B1.send("connect 2")
    check(bool(B1.wait(r"LINK refused: a LoRa connection is in progress", 2,
                       s1)), "ESP-NOW still refused during LoRa")
    s2 = B2.mark()
    B1.press("cancel", 0.3)
    check(bool(B2.wait(r"LORA LINK disconnected from 1", 8, s2)),
          "LoRa disconnect")
    wait_idle(B1, B2)


def test_esp_loss_lora_up():
    print("X6 ESP-NOW connection lost; LoRa session survives", flush=True)
    if not check(esp_connect(B1, B2), "ESP-NOW connected 1-2"):
        return
    s1 = B1.mark()
    t0 = time.monotonic()
    B2.send("radio 1 1")
    m = B1.wait(r"^LINK disconnected from 2 .*", 12, s1)
    check(bool(m), f"board 1 drops ESP-NOW after {time.monotonic() - t0:.1f} "
                   f"s ({m.group(0)[30:80] if m else 'none'})")
    B2.send("radio 11 1")
    time.sleep(1)
    wait_idle(B1, B2)
    check(lora_connect(B1, B2), "LoRa connect right after, same sessions")
    s2 = B2.mark()
    B1.press("cancel", 0.3)
    B2.wait(r"LORA LINK disconnected", 8, s2)
    wait_idle(B1, B2)


def test_lora_mute_during_esp():
    print("X7 LoRa silent during an ESP-NOW connection: voice unaffected",
          flush=True)
    if not check(esp_connect(B1, B2), "ESP-NOW connected 1-2"):
        return
    time.sleep(1.2)
    late = {}
    for mode in ("muted", "live", "muted", "live"):
        flag = "on" if mode == "muted" else "off"
        B1.send(f"lora mute {flag}")
        B2.send(f"lora mute {flag}")
        time.sleep(0.5)
        for sender, receiver, name in ((B1, B2, "1->2"), (B2, B1, "2->1")):
            for _ in range(2):
                rx = send_voice(sender, receiver, 10,
                                f"voice {name}, LoRa {mode}")
                late.setdefault(mode, []).append(
                    int(rx["late"]) if rx else -1)
    B1.send("lora mute off")
    B2.send("lora mute off")
    print(f"  late frames per 500-frame stream: {late}", flush=True)
    for board in BOARDS:
        board.note(f"late frames per stream: {late}")
    screen1, _ = B1.ui()
    check(screen1 == "connected", f"ESP-NOW still connected ({screen1})")
    B1.press("cancel", 0.3)
    B2.wait(r"^LINK disconnected", 4)
    wait_idle(B1, B2)


def test_request_anywhere():
    print("X8 requests over either radio show on any screen", flush=True)
    to_list(B2, "espnow", 1)
    to_list(B1, "lora", 2)
    s2 = B2.mark()
    B1.press("ok", 0.1)
    check(bool(B2.wait(r"UI screen=request", 6, s2)),
          "LoRa request shows while board 2 is on the ESP-NOW list")
    time.sleep(0.8)
    B2.press("cancel", 0.3)
    wait_idle(B1, B2)
    to_list(B2, "lora", 1)
    to_list(B1, "espnow", 2)
    s2 = B2.mark()
    B1.press("ok", 0.1)
    check(bool(B2.wait(r"UI screen=request", 6, s2)),
          "ESP-NOW request shows while board 2 is on the LoRa list")
    screen, _ = B2.ui()
    m = B2.find(r"UI \| screen request \| Board 1 (\S+)", s2)[1]
    time.sleep(0.8)
    B2.press("cancel", 0.3)
    wait_idle(B1, B2)


def test_reboot_esp():
    print("X9 reboot during ESP-NOW connection: both sessions come back",
          flush=True)
    if not check(esp_connect(B1, B2), "ESP-NOW connected 1-2"):
        return
    s1 = B1.mark()
    B2.send("reboot")
    m = B1.wait(r"^LINK (disconnected from 2|2 \(Board 2\))", 15, s1)
    check(bool(m), f"board 1 ends ESP-NOW ({m.group(0)[:60] if m else ''})")
    check(bool(B1.wait(r"^SESSION established with 2", 20, s1)),
          "ESP-NOW session again")
    check(bool(B1.wait(r"LORA SESSION established with 2", 20, s1)),
          "LoRa session again")
    wait_idle(B1, B2)
    check(lora_connect(B2, B1), "rebooted board 2 connects over LoRa")
    B2.press("cancel", 0.3)
    B1.wait(r"LORA LINK disconnected", 8)
    wait_idle(B1, B2)
    check(esp_connect(B2, B1), "and over ESP-NOW")
    B2.press("cancel", 0.3)
    B1.wait(r"^LINK disconnected", 4)
    wait_idle(B1, B2)


TESTS = {
    "session": test_session,
    "decline": test_decline,
    "cancel": test_cancel_asking,
    "guard": test_guard,
    "glare": test_glare,
    "busy": test_busy,
    "texts": test_texts_outside,
    "noanswer": test_no_answer,
    "loss": test_link_loss,
    "reconnect": test_reconnect_texts,
    "reboot": test_reboot,
    "esp": test_esp_basic,
    "espedge": test_esp_edges,
    "switch": test_switch,
    "crossglare": test_cross_glare,
    "outofrange": test_lora_out_of_espnow,
    "esploss": test_esp_loss_lora_up,
    "mutevoice": test_lora_mute_during_esp,
    "anywhere": test_request_anywhere,
    "espreboot": test_reboot_esp,
}


def main():
    global B1, B2, BOARDS
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    run, port1, port2 = sys.argv[1:4]
    names = sys.argv[4:] or list(TESTS)
    EVIDENCE.mkdir(exist_ok=True)
    start = time.monotonic()
    B1 = Board("board1", port1, EVIDENCE / f"{run}_board1.txt", start)
    time.sleep(2)
    B2 = Board("board2", port2, EVIDENCE / f"{run}_board2.txt", start)
    BOARDS = (B1, B2)
    try:
        if names[0] != "session":
            B1.wait(r"LORA SESSION established with 2", 30, 0)
        for name in names:
            TESTS[name]()
    finally:
        passed = sum(1 for ok, _ in results if ok)
        print(f"{passed}/{len(results)} checks passed")
        for board in BOARDS:
            board.close()
    return 0 if passed == len(results) else 1


B1 = B2 = None
BOARDS = ()

if __name__ == "__main__":
    sys.exit(main())
