"""ESP-MESH tests for two boards, driven over serial, reacting to what each
board logs. Board 1 is the preferred mesh root
(network_config::mesh_root_id), board 2 joins it; if the root goes down the
other takes over (root failover). No router anywhere.

The script reboots both boards over serial first (`reboot`), board 1 two
seconds before board 2. Logs go to evidence/<run>_board1.txt and
_board2.txt; tools/check_streams.py can compare their voice streams
afterwards.

    python tools/mesh_test.py mesh1 COM5 COM6 [TEST ...]
    ~/.platformio/penv/bin/python tools/mesh_test.py mesh1 \\
        /dev/cu.usbmodem5B5F0216001 /dev/cu.usbmodem5B5F0215971

Port 1 must be board 1 (the root). TEST names are the keys of TESTS below
(default: all, in that order).
"""
import sys
import time

# The same serial helper as the LoRa edge tests (tools/lora_edge_test.py).
from lora_edge_test import EVIDENCE, Board

results = []
B1 = B2 = None
BOARDS = ()


def check(ok, what):
    results.append((ok, what))
    print(f"  {'PASS' if ok else 'FAIL'}  {what}", flush=True)
    for board in BOARDS:
        board.note(f"{'PASS' if ok else 'FAIL'} {what}")
    return ok


def ui_line(board, pattern, timeout=2.0):
    since = board.mark()
    board.send("ui")
    return board.wait(r"UI \| screen " + pattern, timeout, since)


def mesh_connect(a, b, a_id, b_id):
    """a asks b over the mesh with serial commands; b accepts."""
    s_a, s_b = a.mark(), b.mark()
    a.send(f"mesh connect {b_id}")
    if not b.wait(rf"MESH LINK request \| {a_id} .* over ESP-MESH", 8, s_b):
        return False
    b.send("accept")
    return bool(a.wait(r"MESH LINK connected", 8, s_a) and
                b.wait(r"MESH LINK connected", 2, s_b))


def mesh_disconnect(a, b):
    s_a, s_b = a.mark(), b.mark()
    a.send("disconnect")
    return bool(a.wait(r"MESH LINK disconnected", 8, s_a) and
                b.wait(r"MESH LINK disconnected", 8, s_b))


def wait_idle(*boards):
    for board in boards:
        for _ in range(10):
            if ui_line(board, r"(menu|espnow|mesh|lora|volume|status)\b"):
                break
            time.sleep(0.7)


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


def to_mesh_list(board, peer):
    """Brings `board` to the ESP-MESH list with Board `peer` selected."""
    want_row = f"Connect Board {peer}"
    for _ in range(16):
        screen, label = board.ui()
        if screen == "disconnected":
            time.sleep(1.0)
        elif screen == "mesh":
            if label == want_row:
                return True
            board.press("down")
        elif screen == "menu":
            if label == "ESP-MESH":
                board.press("ok", 0.6)
            else:
                board.press("down")
        else:
            board.press("cancel", 0.6)
    return False


def mesh_role(board):
    """(role, root board ID) from the board's `mesh` line, or (None, 0)."""
    since = board.mark()
    board.send("mesh")
    match = board.wait(r"MESH \| [^|]+ \| (root|node) layer \d+ \| root Board "
                       r"(\d+)", 3, since)
    return (match.group(1), int(match.group(2))) if match else (None, 0)


def wait_any(waits, timeout):
    """First of [(board, pattern, since)] to match: (board, match)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for board, pattern, since in waits:
            _, match = board.find(pattern, since)
            if match:
                return board, match
        time.sleep(0.1)
    return None, None


def test_join():
    print("M1 the mesh forms without a router", flush=True)
    check(bool(B1.wait(r"MESH started \| looking for a mesh", 20, 0)),
          "board 1 looks for a mesh first")
    check(bool(B1.wait(r"MESH started as root", 30, 0)),
          "board 1 (preferred root) becomes the root after its turn")
    check(bool(B2.wait(r"MESH started \| looking for a mesh", 20, 0)),
          "board 2 starts as a node")
    check(bool(B2.wait(r"MESH joined \| parent 1 \(Board 1\)", 60, 0)),
          "board 2 joins board 1")
    check(bool(B1.wait(r"MESH child joined: 2 \(Board 2\)", 10, 0)),
          "board 1 sees board 2 join")
    check(bool(B1.wait(r"MESH SESSION established with 2", 30, 0) and
               B2.wait(r"MESH SESSION established with 1", 5, 0)),
          "mesh session made on both boards")
    check(bool(B1.wait(r"MESH PEER 2 \(Board 2\) online", 15, 0) and
               B2.wait(r"MESH PEER 1 \(Board 1\) online", 15, 0)),
          "each board sees the other online over the mesh")
    _, failed = B1.find(r"MESH .*failed")
    check(failed is None, "no mesh start-up failure on board 1")
    _, failed = B2.find(r"MESH .*failed")
    check(failed is None, "no mesh start-up failure on board 2")
    check(bool(B1.find(r"^SESSION established with 2")[1]),
          "ESP-NOW session made alongside the mesh")
    _, second_root = B2.find(r"MESH started as root")
    check(second_root is None, "board 2 never became a second root")


def test_connect():
    print("M2 connect, refuse a second connection, disconnect", flush=True)
    wait_idle(B1, B2)
    check(mesh_connect(B1, B2, 1, 2), "board 1 connects to board 2 over mesh")
    check(bool(ui_line(B1, r"connected \| ESP-MESH")),
          "board 1 shows the connected screen for ESP-MESH")
    check(bool(ui_line(B2, r"connected \| ESP-MESH")),
          "board 2 shows the connected screen for ESP-MESH")
    since = B1.mark()
    B1.send("connect 2")
    check(bool(B1.wait(r"^LINK refused: an ESP-MESH connection is in "
                       r"progress", 3, since)),
          "ESP-NOW connect refused during the mesh connection")
    since = B1.mark()
    B1.send("lora connect 2")
    refused = B1.wait(r"LORA LINK refused: (an ESP-MESH connection|radio)",
                      3, since)
    check(bool(refused), "LoRa connect refused during the mesh connection"
          + (" (LoRa radio not ready)" if refused and
             refused.group(1) == "radio" else ""))
    check(mesh_disconnect(B2, B1), "board 2 disconnects; both drop it")


def test_hello():
    print("M3 hello both ways", flush=True)
    wait_idle(B1, B2)
    if not check(mesh_connect(B1, B2, 1, 2), "connected over mesh"):
        return
    since = B2.mark()
    B1.send("hello")
    check(bool(B2.wait(r'MESH TEXT from 1 \(Board 1\): "Hello from Board 1"',
                       5, since)), "board 2 gets board 1's hello")
    time.sleep(1.2)
    since = B1.mark()
    B2.send("hello")
    check(bool(B1.wait(r'MESH TEXT from 2 \(Board 2\): "Hello from Board 2"',
                       5, since)), "board 1 gets board 2's hello")
    mesh_disconnect(B1, B2)


def test_voice():
    print("M4 voice over the mesh, both ways", flush=True)
    wait_idle(B1, B2)
    if not check(mesh_connect(B1, B2, 1, 2), "connected over mesh"):
        return
    check(voice(B1, B2, 5, "mesh"), "5 s test stream 1 -> 2 intact")
    check(voice(B2, B1, 5, "mesh"), "5 s test stream 2 -> 1 intact")
    check(voice(B1, B2, 10, "mesh"), "10 s test stream 1 -> 2 intact")
    mesh_disconnect(B1, B2)


def test_dpad():
    print("M5 the same from the D-pad (serial press)", flush=True)
    wait_idle(B1, B2)
    if not check(to_mesh_list(B1, 2), "board 1 on the ESP-MESH list"):
        return
    s1, s2 = B1.mark(), B2.mark()
    B1.press("ok", 0.1)
    check(bool(B2.wait(r"MESH LINK request \| 1", 8, s2)),
          "board 2 shows the mesh request")
    time.sleep(0.8)  # Past the request screen's 500 ms guard.
    B2.press("ok", 0.1)
    if not check(bool(B1.wait(r"MESH LINK connected", 8, s1)),
                 "connected from the D-pad"):
        return
    since = B2.mark()
    B1.press("right")
    check(bool(B2.wait(r"MESH TEXT from 1", 5, since)), "RIGHT says hello")
    time.sleep(1.2)
    since = B2.mark()
    B1.send("press ok 3000")  # Hold OK 3 s: push-to-talk.
    check(bool(B2.wait(r"RX_START .* via=mesh", 5, since)),
          "holding OK talks over the mesh")
    check(bool(B2.wait(r"RX_END .* intact=yes", 12, since)),
          "board 2 played the whole stream")
    time.sleep(2.0)
    s1, s2 = B1.mark(), B2.mark()
    B1.press("cancel", 0.3)
    check(bool(B1.wait(r"MESH LINK disconnected", 5, s1) and
               B2.wait(r"MESH LINK disconnected", 8, s2)),
          "CANCEL disconnects both")


def test_loss():
    print("M6 link loss: board 2 goes silent (mesh mute)", flush=True)
    wait_idle(B1, B2)
    if not check(mesh_connect(B1, B2, 1, 2), "connected over mesh"):
        return
    s1, s2 = B1.mark(), B2.mark()
    B2.send("mesh mute on")
    lost1 = B1.wait(r"MESH LINK disconnected", 15, s1)
    lost2 = B2.wait(r"MESH LINK disconnected", 15, s2)
    check(bool(lost1 and lost2), "both drop the connection within 15 s")
    since = B1.mark()
    B2.send("mesh mute off")
    check(bool(B1.wait(r"MESH PEER 2 \(Board 2\) online", 15, since)),
          "board 2 back online after unmuting")
    wait_idle(B1, B2)
    check(mesh_connect(B2, B1, 2, 1), "board 2 can connect again")
    mesh_disconnect(B2, B1)


def test_espnow():
    print("M7 ESP-NOW still works in this build", flush=True)
    wait_idle(B1, B2)
    s1, s2 = B1.mark(), B2.mark()
    B1.send("connect 2")
    if not check(bool(B2.wait(r"^LINK request \| 1", 8, s2)),
                 "board 2 gets the ESP-NOW request"):
        return
    B2.send("accept")
    check(bool(B1.wait(r"^LINK connected to 2", 8, s1)),
          "connected over ESP-NOW")
    check(voice(B1, B2, 5, "espnow"), "5 s ESP-NOW test stream intact")
    since = B2.mark()
    B2.send("mesh connect 1")
    check(bool(B2.wait(r"MESH LINK refused: an ESP-NOW connection", 3,
                       since)), "mesh connect refused during ESP-NOW")
    s1, s2 = B1.mark(), B2.mark()
    B1.send("disconnect")
    check(bool(B1.wait(r"^LINK disconnected", 5, s1) and
               B2.wait(r"^LINK disconnected", 5, s2)),
          "ESP-NOW disconnect")


def test_reboot():
    print("M8 board 2 reboots during a mesh connection", flush=True)
    wait_idle(B1, B2)
    if not check(mesh_connect(B1, B2, 1, 2), "connected over mesh"):
        return
    s1, s2 = B1.mark(), B2.mark()
    B2.send("reboot")
    check(bool(B1.wait(r"MESH LINK disconnected", 20, s1)),
          "board 1 ends the connection")
    check(bool(B2.wait(r"MESH joined \| parent 1", 60, s2)),
          "board 2 rejoins the mesh")
    check(bool(B1.wait(r"MESH SESSION established with 2", 30, s1)),
          "new mesh session")
    wait_idle(B1, B2)
    check(mesh_connect(B2, B1, 2, 1), "rebooted board 2 connects again")
    mesh_disconnect(B2, B1)


def test_root_down():
    print("M9 the root goes down: the other board takes over, the root comes "
          "back as a node", flush=True)
    wait_idle(B1, B2)
    role1, root = mesh_role(B1)
    if not check(root in (1, 2), f"a root is up (board 1 is {role1}, root "
                                 f"is Board {root})"):
        return
    down, up = (B1, B2) if root == 1 else (B2, B1)
    down_id, up_id = (1, 2) if down is B1 else (2, 1)
    s_down, s_up = down.mark(), up.mark()
    down.send("mesh off")  # As if the root lost power.
    check(bool(up.wait(r"MESH no mesh with a root .*becoming the root", 45,
                       s_up)),
          f"board {up_id} takes over after its turn")
    check(bool(up.wait(r"MESH started as root", 10, s_up)),
          f"board {up_id} is the root now")
    s_down = down.mark()
    down.send("mesh on")
    joined = down.wait(rf"MESH joined \| parent {up_id} ", 75, s_down)
    check(bool(joined), f"board {down_id} comes back as a node under board "
                        f"{up_id}, not as a second root")
    check(bool(down.wait(rf"MESH PEER {up_id} \(Board {up_id}\) online", 20,
                         s_down)),
          f"board {down_id} sees board {up_id} online again")
    wait_idle(B1, B2)
    if check(mesh_connect(down, up, down_id, up_id),
             "connect over the mesh after the takeover"):
        check(voice(down, up, 5, "mesh"), "voice intact after the takeover")
        mesh_disconnect(down, up)


def test_split():
    print("M10 two roots: the one nobody joins steps down and joins the "
          "other", flush=True)
    wait_idle(B1, B2)
    _, root = mesh_role(B1)
    node, node_id = (B2, 2) if root == 1 else (B1, 1)
    s1, s2 = B1.mark(), B2.mark()
    node.send("mesh root")  # Leaves the mesh and forms a second one.
    check(bool(node.wait(r"MESH started as root", 10,
                         s2 if node is B2 else s1)),
          f"board {node_id} forced to be a second root")
    board, match = wait_any([
        (B1, r"MESH joined \| parent 2 ", s1),
        (B2, r"MESH joined \| parent 1 ", s2)], 100)
    if not check(bool(match), "one root steps down and the boards are one "
                              "mesh again (within 100 s)"):
        return
    stepped = B1 if board is B1 else B2
    _, lonely = stepped.find(r"MESH root with nobody joined",
                             s1 if stepped is B1 else s2)
    check(bool(lonely), "it stepped down because nobody had joined it")
    check(bool(B1.wait(r"MESH PEER 2 \(Board 2\) online", 20, s1) and
               B2.wait(r"MESH PEER 1 \(Board 1\) online", 20, s2)),
          "both online over the mesh again")
    wait_idle(B1, B2)
    if check(mesh_connect(B1, B2, 1, 2), "connect after the merge"):
        mesh_disconnect(B1, B2)


TESTS = {
    "join": test_join,
    "connect": test_connect,
    "hello": test_hello,
    "voice": test_voice,
    "dpad": test_dpad,
    "loss": test_loss,
    "espnow": test_espnow,
    "reboot": test_reboot,
    "rootdown": test_root_down,
    "split": test_split,
}


def main():
    global B1, B2, BOARDS
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    run, port1, port2 = sys.argv[1:4]
    names = sys.argv[4:] or list(TESTS)
    unknown = [name for name in names if name not in TESTS]
    if unknown:
        sys.exit(f"unknown test {', '.join(unknown)}; choose from "
                 f"{' '.join(TESTS)}")
    EVIDENCE.mkdir(exist_ok=True)
    start = time.monotonic()
    B1 = Board("board1", port1, EVIDENCE / f"{run}_board1.txt", start)
    B2 = Board("board2", port2, EVIDENCE / f"{run}_board2.txt", start)
    BOARDS = (B1, B2)
    # Opening a port doesn't reset the board on every OS: reboot both, so
    # each log starts at boot. Board 1 first, as at power-on.
    B1.send("reboot")
    time.sleep(2)
    B2.send("reboot")
    try:
        if names[0] != "join":
            B1.wait(r"MESH PEER 2 \(Board 2\) online", 90, 0)
            B2.wait(r"MESH PEER 1 \(Board 1\) online", 10, 0)
        for name in names:
            TESTS[name]()
    finally:
        passed = sum(1 for ok, _ in results if ok)
        print(f"{passed}/{len(results)} checks passed")
        for board in BOARDS:
            board.close()
    return 0 if results and passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
