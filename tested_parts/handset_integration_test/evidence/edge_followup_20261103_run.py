"""Repeat near-simultaneous dial attempts and connected hello throttling."""
from pathlib import Path
import subprocess
import sys
root = Path(__file__).resolve().parents[1]
cmd = [sys.executable, str(root/'tools/capture_logs.py'), 'edge_followup_20261103',
       'b1=/dev/cu.usbmodem5B5F0216001', 'b2=/dev/cu.usbmodem5B5F0215971',
       '--baud','921600','--duration','39']
def send(t,b,c):
    cmd.extend(['--send',f'{b}@{t}={c}'])
for t in [5,13,21]:
    for b,peer in [('b1',2),('b2',1)]:
        send(t,b,f'connect {peer}')
        send(t+1,b,'link')
        send(t+2,b,'accept')
        send(t+3,b,'link')
        send(t+4,b,'hello')
        send(t+4.25,b,'hello')
        send(t+5.5,b,'disconnect')
send(29,'b1','connect 2'); send(31,'b2','accept')
for b in ['b1','b2']:
    send(32,b,'hello'); send(32.25,b,'hello'); send(33.5,b,'hello')
    send(34.5,b,'ui'); send(35,b,'disconnect'); send(36,b,'release')
    send(37,b,'link'); send(38,b,'report')
raise SystemExit(subprocess.call(cmd,cwd=root))
