#!/usr/bin/env python3
"""Capture untouched stereo I2S words, check SHA256, and make listening WAVs."""
import argparse
from array import array
from datetime import datetime
import hashlib
import json
import math
from pathlib import Path
import sys
import time
import wave


def write_wav(path, samples, rate, channels):
    words = array('h', samples)
    if sys.byteorder != 'little':
        words.byteswap()
    with wave.open(str(path), 'wb') as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(words.tobytes())


def export_capture(raw, rate, directory):
    if not raw or len(raw) % 8:
        raise ValueError('Capture must contain complete stereo 32-bit frames')
    words = array('i')
    if words.itemsize != 4:
        raise RuntimeError('This script needs 32-bit signed integers')
    words.frombytes(raw)
    if sys.byteorder != 'little':
        words.byteswap()
    # Standard 24-bit I2S occupies the high bits of each 32-bit slot.
    pcm = [word >> 16 for word in words]
    directory.mkdir(parents=True, exist_ok=False)
    (directory / 'raw-i2s.bin').write_bytes(raw)
    write_wav(directory / 'stereo-unity.wav', pcm, rate, 2)
    report = {'rate': rate, 'frames': len(pcm) // 2, 'sha256': hashlib.sha256(raw).hexdigest()}
    for slot, name in enumerate(('left', 'right')):
        channel = pcm[slot::2]
        count = len(channel)
        mean = sum(channel) / count
        ac = [sample - mean for sample in channel]
        ac_peak = max(abs(sample) for sample in ac)
        gain = min(64.0, 0.8 * 32767 / ac_peak) if ac_peak else 1.0
        normalized = [max(-32768, min(32767, round(sample * gain))) for sample in ac]
        write_wav(directory / (name + '-listen.wav'), normalized, rate, 1)
        report[name] = {
            'min': min(channel), 'max': max(channel), 'mean': mean,
            'rms': math.sqrt(sum(sample * sample for sample in channel) / count),
            'ac_rms': math.sqrt(sum(sample * sample for sample in ac) / count),
            'full_scale_samples': sum(sample in (-32768, 32767) for sample in channel),
            'nonzero_samples': sum(sample != 0 for sample in channel),
            'listen_gain': gain,
            'first_16_raw_words_hex': [f'{word & 0xffffffff:08x}' for word in words[slot::2][:16]],
        }
    (directory / 'stats.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    import serial  # PlatformIO's Python includes pyserial.
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--rate', type=int, choices=(16000, 48000), default=16000)
    parser.add_argument('--reset', action='store_true', help='Reset UART/COM-connected board')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    destination = args.output or Path('mic_capture/captures') / (datetime.now().strftime('%Y%m%d-%H%M%S-%f') + f'-{args.rate}')
    if destination.exists():
        parser.error(f'Output already exists: {destination}')
    conn = serial.Serial(port=None, baudrate=921600, timeout=1, write_timeout=10)
    conn.dtr = False
    conn.rts = False
    conn.port = args.port
    conn.open()
    with conn:
        def wait_line(prefix, timeout=15):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                line = conn.readline().decode('ascii', errors='replace').strip()
                if line.startswith('ERROR'):
                    raise RuntimeError(line)
                if line.startswith(prefix):
                    return line
                if line.startswith(('CAPTURING', 'MIC_CAPTURE_READY', 'Commands:')):
                    print(line, flush=True)
            raise TimeoutError(f'No {prefix} response; check firmware, baud, and monitor ownership')

        if args.reset:
            conn.rts = True
            time.sleep(0.15)
            conn.rts = False
        time.sleep(2)
        conn.write(b'status\n')
        print(wait_line('MIC_CAPTURE_READY'), flush=True)
        print('Keep the speaker quiet. Speak near the mic for the next three seconds.', flush=True)
        conn.write(f'capture {args.rate}\n'.encode())
        header = wait_line('RAW ').split()
        if len(header) != 5:
            raise RuntimeError('Malformed capture header')
        rate, frames, length = map(int, header[1:4])
        if rate != args.rate or frames != rate * 3 or length != frames * 8 or len(header[4]) != 64:
            raise RuntimeError('Unexpected capture format/length')
        conn.write(b'GET\n')
        raw = bytearray()
        deadline = time.monotonic() + 60
        while len(raw) < length and time.monotonic() < deadline:
            raw.extend(conn.read(min(16384, length - len(raw))))
        if len(raw) != length:
            raise RuntimeError(f'Truncated capture: {len(raw)}/{length} bytes')
        wait_line('END')
        if hashlib.sha256(raw).hexdigest() != header[4]:
            raise RuntimeError('Serial transfer checksum mismatch; capture was not saved')
    report = export_capture(raw, rate, destination)
    print(json.dumps(report, indent=2))
    print(f'Saved verified capture: {destination.resolve()}')
    print('Listen files have DC removed and fixed normalization (up to 64x); stereo-unity.wav is unmodified PCM16.')


if __name__ == '__main__':
    main()
