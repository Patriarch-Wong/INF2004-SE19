#!/usr/bin/env python3
"""Mutate temporary copies only. Every listed mutation must fail a test."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MUTATIONS = [
    ('src/voice.cpp', 'info->sequence < info->frames', 'info->sequence > info->frames'),
    ('src/voice.cpp', 'if (arrivals[s] == missing)', 'if (false)'),
    ('src/voice.cpp', 'depth + (static_cast<int64_t>(s) - anchor) * interval',
     '0 * depth + (static_cast<int64_t>(s) - anchor) * interval'),
    ('src/voice.cpp', 'e.valid = false;', 'e.valid = true;'),
    ('src/voice.cpp', 'values[(n - 1) / 2]', 'values[n - 1]'),
    ('src/sniff_frame.cpp', 'b[pos + 5] == 4', 'b[pos + 5] == 5'),
    ('tools/analyse_capture.py', 'not marked and not collisions', 'not marked'),
    ('tools/analyse_capture.py', 'completeness >= .95', 'completeness < .95'),
]
for file, before, after in MUTATIONS:
    with tempfile.TemporaryDirectory(prefix='stage5-mutation-') as directory:
        target = Path(directory)
        for folder in ('src', 'include', 'tests', 'tools'):
            shutil.copytree(ROOT / folder, target / folder,
                            ignore=shutil.ignore_patterns('e2e_keys.h', '__pycache__'))
        path = target / file
        text = path.read_text()
        assert before in text, (file, before)
        path.write_text(text.replace(before, after, 1))
        result = subprocess.run(['sh', str(target / 'tests/run_voice_tests.sh')],
                                capture_output=True, text=True)
        # Compilation failures don't count as a behavioural mutation kill.
        assert 'error:' not in result.stderr, result.stderr
        assert result.returncode != 0, f'SURVIVED: {before}'
        print(f'Killed: {file}: {before}')
