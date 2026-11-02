"""Direct hardware smoke test. Close monitors first; never flashes or deliberately toggles reset. Opening a port may reset some USB bridges."""
import argparse
import time
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('board_a')
parser.add_argument('board_b')
parser.add_argument("--b-first", action="store_true", help="Test B -> A before A transmits")
args = parser.parse_args()
ports = []
logs = [bytearray(), bytearray()]

def capture(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        for i, port in enumerate(ports):
            data = port.read(port.in_waiting)
            if data:
                logs[i].extend(data)
                print(f'[{"AB"[i]}] {data.decode("utf-8", errors="replace")}', end='', flush=True)
        time.sleep(0.01)

try:
    for path in (args.board_a, args.board_b):
        port = serial.Serial(port=None, baudrate=115200, timeout=0)
        port.dtr = False
        port.rts = False
        port.port = path
        port.open()
        ports.append(port)
    capture(6)
    passed = True
    token = str(time.time_ns())
    cases = [(0, 1, 101), (1, 0, 102)]
    if args.b_first:
        cases.reverse()
    for sender, receiver, peer in cases:
        message = f'chat-check-{token}-{"AB"[sender]}'
        start = len(logs[receiver])
        print(f'\nSEND {"AB"[sender]}: {message}', flush=True)
        ports[sender].write((message + '\n').encode())
        ports[sender].flush()
        capture(4)
        result = logs[receiver][start:].decode('utf-8', errors='replace')
        ok = any(f'RX from {peer} #' in line and f': {message}' in line for line in result.splitlines())
        print(f'\n{"PASS" if ok else "FAIL"}: {"AB"[sender]} -> {"AB"[receiver]}', flush=True)
        passed &= ok
    raise SystemExit(0 if passed else 1)
finally:
    for port in ports:
        port.close()
