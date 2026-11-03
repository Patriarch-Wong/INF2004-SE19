"""Hardware regression: reject invalid IDs, then call and send both ways."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
cmd = [sys.executable, str(root/'tools/capture_logs.py'), 'c10_fixed_20261103',
       'b1=/dev/cu.usbmodem5B5F0216001', 'b2=/dev/cu.usbmodem5B5F0215971',
       '--baud', '921600', '--duration', '45']
def send(t, board, text):
    cmd.extend(['--send', f'{board}@{t}={text}'])
def both(t, text):
    for board in ('b1', 'b2'):
        send(t, board, text)

invalid = ['258', '257', '514', '-254', '-255', '+2', '0', '6', '255',
           '256', '4294967298', '18446744073709551618',
           '9999999999999999999999999999999999999999', '2x', '2 3', '2.0',
           '0x2', '']
for i, value in enumerate(invalid):
    both(4 + i * 0.5, 'connect ' + value)
send(14, 'b1', 'connect 1'); send(14, 'b2', 'connect 2')
both(15, 'connect 4'); both(16, 'connect 5'); both(17, 'connect 3')
both(18, 'link')
send(20, 'b1', 'connect 2'); send(22, 'b2', 'accept')
both(23, 'hello'); send(24, 'b1', 'test 2')
both(27, 'connect 258'); both(28, 'link'); send(29, 'b1', 'disconnect')
send(31, 'b2', 'connect \t001 '); send(33, 'b1', 'accept')
send(35, 'b2', 'test 2'); both(36, 'hello'); both(39, 'link')
send(40, 'b2', 'disconnect'); both(41, 'r'); both(42, 'link')
both(43, 'report'); both(44, 'ui')
raise SystemExit(subprocess.call(cmd, cwd=root))
