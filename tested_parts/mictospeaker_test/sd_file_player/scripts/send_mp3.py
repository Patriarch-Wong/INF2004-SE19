#!/usr/bin/env python3
"""Copy an MP3 to SD over serial, verify SHA256, and start playback."""
import argparse
import hashlib
import pathlib
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--reset', action='store_true', help='Reset a UART/COM-connected ESP32 before transfer')
parser.add_argument('--start-only', action='store_true', help='Release the port after playback progress is confirmed')
parser.add_argument('file', nargs='?', default='track.mp3')
args = parser.parse_args()
data = pathlib.Path(args.file).read_bytes()
sha = hashlib.sha256(data).hexdigest()
conn = serial.Serial(port=None, baudrate=921600, timeout=1, write_timeout=15)
# Avoid holding EN/BOOT asserted when opening the USB-to-UART bridge.
conn.dtr = False
conn.rts = False
conn.port = args.port
conn.open()
with conn:
    def wait_for(prefixes, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = conn.readline().decode('utf-8', errors='replace').strip()
            if not line:
                continue
            if line.startswith(('ERROR', 'FAILED')):
                raise RuntimeError(line)
            if line.startswith(prefixes):
                return line
            print(line, flush=True)
        raise TimeoutError(f'No response: {prefixes}')

    if args.reset:
        conn.rts = True
        time.sleep(0.15)
        conn.rts = False
    time.sleep(2)
    conn.write(b'status\n')
    print(wait_for(('SD READY',)), flush=True)
    conn.write(f'put {len(data)} {sha}\n'.encode())
    reply = wait_for(('READY', 'EXISTS'), 60)
    print(reply, flush=True)
    if reply == 'READY':
        for offset in range(0, len(data), 4096):
            chunk = data[offset:offset + 4096]
            conn.write(chunk)
            ack = wait_for(('ACK ',))
            if ack != f'ACK {offset + len(chunk)}':
                raise RuntimeError(f'Unexpected acknowledgement: {ack}')
            if offset % (4096 * 128) == 0:
                print(f'Transferred {offset + len(chunk)}/{len(data)} bytes', flush=True)
        reply = wait_for(('SAVED ',), 60)
        if reply != f'SAVED {len(data)} {sha}':
            raise RuntimeError(f'Unexpected verification result: {reply}')
        print(reply, flush=True)
    conn.write(b'play\n')
    print(wait_for(('AUDIO ',), 30), flush=True)
    if args.start_only:
        print(wait_for(('PROGRESS ', 'DONE ', 'STOPPED '), 30), flush=True)
        print('Serial port released; board continues playback.', flush=True)
    else:
        print(wait_for(('DONE ', 'STOPPED '), 1200), flush=True)
