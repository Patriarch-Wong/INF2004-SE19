"""Compare our codec bit-for-bit with Python 3.9-3.12's independent audioop codec.

Usage: /usr/bin/python3 checks/adpcm_reference_test.py /tmp/adpcm_reference_driver
"""
import audioop
import math
import random
import struct
import subprocess
import sys

rng = random.Random(43434)
patterns = [
    [0] * 320,
    [-32768, 32767] * 160,
    [int(12000 * math.sin(2 * math.pi * 500 * i / 16000)) for i in range(320)],
    [rng.randrange(-32768, 32768) for _ in range(320)],
]
fixtures = []
for index in range(89):
    for pattern in patterns:
        initial = (rng.randrange(-32768, 32768), index)
        pcm = struct.pack('<320h', *pattern)
        fixtures.append((initial, pcm))
payload = b''.join(struct.pack('<hBB', *state, 0) + pcm for state, pcm in fixtures)
result = subprocess.run([sys.argv[1]], input=payload, stdout=subprocess.PIPE, check=True).stdout
record_size = 180 + 640 + 3
assert len(result) == len(fixtures) * record_size
for i, (state, pcm) in enumerate(fixtures):
    record = result[i * record_size:(i + 1) * record_size]
    signature, session, seq, rate, count, predictor, index, reserved = struct.unpack('<IIIHHhBB', record[:20])
    assert (signature, rate, count, predictor, index, reserved) == (0x32445541, 16000, 320, *state, 0)
    expected_codes, expected_state = audioop.lin2adpcm(pcm, 2, state)
    assert record[20:180] == expected_codes, ('encoder mismatch', i, state)
    expected_pcm, decoded_state = audioop.adpcm2lin(expected_codes, 2, state)
    assert record[180:820] == expected_pcm, ('decoder mismatch', i, state)
    assert struct.unpack('<hB', record[820:]) == expected_state == decoded_state
print(f'ADPCM matches independent audioop encoder/decoder for {len(fixtures)} blocks / all 89 step indices.')
