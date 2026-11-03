"""Replay two-board hardware edge cases; opening ports resets the boards."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
events = []
def send(t, board, command):
    events.append((t, board, command))
def both(t, command):
    for b in ('b1', 'b2'):
        send(t, b, command)

both(4, 'hello')
both(5, 'talk 1')
both(6, 'accept')
both(7, 'decline')
for t, c in [(8,'connect 0'), (9,'connect 1'), (10,'connect 4'), (11,'connect 258'), (12,'disconnect')]:
    send(t,'b1',c)
send(14,'b1','connect 2'); send(14,'b2','connect 1')
both(15,'link'); send(16,'b2','accept'); both(17,'link')
both(18,'hello'); both(18.25,'hello')
both(20,'hold ok'); both(22,'release'); both(25,'report')
both(26,'hold ok'); both(28,'release'); both(31,'report')
send(32,'b1','test 3'); send(33,'b2','hello')
send(37,'b2','test 3'); send(38,'b1','hello')
both(42,'r')
send(43,'b1','hold ok'); send(44,'b2','disconnect')
send(44.5,'b2','connect 1'); send(45.3,'b1','accept')
send(46,'b2','test 2'); send(46.5,'b1','release')
both(50,'r'); both(51,'link')
send(52,'b2','radio 1 1'); both(56,'link'); both(57,'ui')
send(58,'b1','connect 2'); send(60,'b2','radio 11 1')
send(67,'b1','connect 2'); send(69,'b2','accept'); both(70,'link')
send(71,'b2','reboot'); both(77,'link'); both(78,'ui')
send(80,'b1','connect 2'); send(81,'b2','reboot')
both(87,'link'); both(88,'ui')
send(90,'b1','connect 2'); send(92,'b2','decline'); both(94,'link')
send(96,'b1','connect 2'); send(97,'b1','disconnect'); both(99,'link')
send(101,'b1','connect 2'); both(133,'link')
send(135,'b2','connect 1'); send(137,'b1','accept')
send(139,'b1','test 5'); send(147,'b2','test 5')
both(155,'r'); both(156,'report'); both(157,'disconnect')
both(159,'hello'); both(160,'link'); both(161,'ui')
cmd = [sys.executable, str(root/'tools/capture_logs.py'), 'edge_20261103',
       'b1=/dev/cu.usbmodem5B5F0216001', 'b2=/dev/cu.usbmodem5B5F0215971',
       '--baud','921600','--duration','163']
for t,b,c in sorted(events):
    cmd.extend(['--send', f'{b}@{t}={c}'])
raise SystemExit(subprocess.call(cmd, cwd=root))
