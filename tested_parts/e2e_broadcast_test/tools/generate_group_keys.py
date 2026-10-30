#!/usr/bin/env python3
"""Create local bench group keys once; never print their values."""
from pathlib import Path
import secrets
path = Path(__file__).resolve().parents[1] / 'include/group_keys.h'
text = '#pragma once\n#include <stdint.h>\nnamespace group_keys {\n'
for name in ('audio', 'hop'):
    text += f'constexpr uint8_t {name}[16] = {{' + ', '.join(
        f'0x{b:02X}' for b in secrets.token_bytes(16)) + '};\n'
text += '}\n'
with path.open('x') as output:
    output.write(text)
path.chmod(0o600)
print('Created include/group_keys.h; build the same image for all three boards.')
