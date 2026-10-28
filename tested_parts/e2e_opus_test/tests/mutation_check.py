#!/usr/bin/env python3
"""Mutate temporary copies only. Every listed mutation must fail a test."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MUTATIONS = [
    # Stage 6: Opus payload, signal, comparison and playback.
    ('src/opus_voice.cpp', 'static_cast<int64_t>(arrival) <= depth_us +',
     'static_cast<int64_t>(arrival) < depth_us +'),
    ('src/opus_voice.cpp', 'if (length != size - header_size || length > max_packet_size)',
     'if (length > max_packet_size)'),
    ('src/opus_voice.cpp', 'if (position < lookahead)', 'if (false)'),
    ('src/opus_voice.cpp', '(crc >> 1) ^ (0xEDB88320u', '(crc >> 1) ^ (0x04C11DB7u'),
    ('src/opus_voice.cpp', '((3 * u * u) << 32) / 1280000', '((3 * u * u) << 32) / 1280'),
    ('src/opus_player.cpp', 'return tried ? outcome::decode_failed : outcome::concealed;',
     'return tried && false ? outcome::decode_failed : outcome::concealed;'),
    ('src/opus_player.cpp', 'const bool use = depth_us == in_order ?',
     'const bool use = true ?'),
    ('tools/analyse_capture.py', 'if plain else not seen', 'if plain else True'),
    ('tools/analyse_capture.py', 'with_packet == len(voice) > 0', 'with_packet > 0'),
    ('tools/analyse_capture.py', 'len(sent) == 3000 and matching == 3000', 'len(sent) == 3000'),
    # Stage 5 rules still in use.
    ('src/voice.cpp', 'if (arrivals[s] == missing)', 'if (false)'),
    ('src/sniff_frame.cpp', 'b[pos + 5] == 4', 'b[pos + 5] == 5'),
    ('tools/analyse_capture.py', 'not marked and not collisions', 'not marked'),
    ('tools/analyse_capture.py', 'completeness >= .95', 'completeness < .95'),
    ('tools/analyse_capture.py', 'for f in unique_sent:', 'for f in selected:'),
    ('tools/analyse_capture.py',
     'not any(len(v) > 1 for v in by_header.values())', 'True'),
]
for file, before, after in MUTATIONS:
    with tempfile.TemporaryDirectory(prefix='stage6-mutation-') as directory:
        target = Path(directory)
        for folder in ('src', 'include', 'tests', 'tools'):
            shutil.copytree(ROOT / folder, target / folder,
                            ignore=shutil.ignore_patterns('e2e_keys.h', '__pycache__'))
        path = target / file
        text = path.read_text()
        assert before in text, (file, before)
        path.write_text(text.replace(before, after, 1))
        result = subprocess.run(['sh', str(target / 'tests/run_opus_tests.sh')],
                                capture_output=True, text=True)
        # Compilation failures don't count as a behavioural mutation kill.
        assert 'error:' not in result.stderr, result.stderr
        assert result.returncode != 0, f'SURVIVED: {before}'
        print(f'Killed: {file}: {before}')
