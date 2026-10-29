#!/usr/bin/env python3
"""Mutate temporary copies only. Every listed mutation must fail a test."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MUTATIONS = [
    # Slot choice.
    ('src/broadcast_receiver.cpp', '        return source::redundant;',
     '        return source::concealed;'),
    ('src/broadcast_receiver.cpp',
     'if (opus_voice::on_time(primary_arrival, sequence, anchor, depth_us,',
     'if (primary_arrival != missing &&\n'
     '        opus_voice::on_time(earliest_arrival, sequence, anchor, depth_us,'),
    # The redundant copy is sequence - 1.
    ('src/broadcast_receiver.cpp', 'store(fields.sequence - 1, false',
     'store(fields.sequence, false'),
    # Stream lock.
    ('src/broadcast_receiver.cpp', 'fields.stream_id == id &&', 'true &&'),
    ('src/broadcast_receiver.cpp', 'fields.sender == from &&', 'true &&'),
    ('src/broadcast_receiver.cpp', 'if (fields.stream_id == last_id)',
     'if (false)'),
    ('src/broadcast_receiver.cpp', 'if (fields.frames != slots.frames)',
     'if (false)'),
    # Copies: first one wins, and copies must agree.
    ('src/broadcast_receiver.cpp', 'const bool first = !slots.seen[sequence];',
     'const bool first = true;'),
    ('src/broadcast_receiver.cpp', 'if (slots.seen[sequence] & kind)',
     'if (false)'),
    # Length, flag and header checks.
    ('src/broadcast_frame.cpp', 'previous_size != rest - 2',
     'previous_size > rest - 2'),
    ('src/broadcast_frame.cpp',
     'return rest ? parse_result::bad_length : parse_result::ok;',
     'return parse_result::ok;'),
    ('src/broadcast_frame.cpp', 'header_size + packet_size > size',
     'header_size + packet_size > size + 1'),
    ('src/broadcast_frame.cpp', '(flags & ~flag_previous) ||', 'false ||'),
    ('src/broadcast_frame.cpp',
     '(!fields.sequence || !valid_packet(previous_size))',
     '(!valid_packet(previous_size))'),
    ('src/broadcast_frame.cpp', 'fields.sequence < fields.frames',
     'fields.sequence <= fields.frames'),
    # Analysis: CRC, loss counts, recovery and verdicts.
    ('tools/analyse_broadcast.py',
     "if c['crc'] != sent_crc.get(c['sequence']):", 'if False:'),
    ('tools/analyse_broadcast.py',
     'lost = sorted(set(range(FRAMES)) - set(primary))',
     'lost = sorted(set(range(FRAMES)) - set(earliest))'),
    ('tools/analyse_broadcast.py', 'if lengths and s == end + 1:',
     'if lengths and s == end + 2:'),
    ('tools/analyse_broadcast.py',
     'unrecovered_next = [s for s in next_arrived if s not in earliest]',
     'unrecovered_next = []'),
    ('tools/analyse_broadcast.py', "v['no_copy_fraction'] < .005",
     "v['no_copy_fraction'] < 1"),
    ('tools/analyse_broadcast.py', 'both, one = a & b, a ^ b',
     'both, one = a & b, a | b'),
    ('tools/analyse_broadcast.py',
     "v['live_missed'] == 0 and v['live_matches_log']",
     "v['live_missed'] == 0"),
    ('tools/analyse_broadcast.py',
     "wrong_slots = sum(p['source'] != expected.get(p['sequence'])",
     "wrong_slots = 0 * sum(p['source'] != expected.get(p['sequence'])"),
    ('tools/analyse_broadcast.py', 'step_ok=max(steps) <= STEP_LIMIT_US',
     'step_ok=True'),
]
for file, before, after in MUTATIONS:
    with tempfile.TemporaryDirectory(prefix='broadcast-mutation-') as directory:
        target = Path(directory)
        for folder in ('src', 'include', 'tests', 'tools'):
            shutil.copytree(ROOT / folder, target / folder,
                            ignore=shutil.ignore_patterns('__pycache__'))
        path = target / file
        text = path.read_text()
        assert before in text, (file, before)
        path.write_text(text.replace(before, after, 1))
        result = subprocess.run(['sh', str(target / 'tests/run_tests.sh')],
                                capture_output=True, text=True)
        # Compilation failures don't count as a behavioural mutation kill.
        assert 'error:' not in result.stderr, result.stderr
        assert result.returncode != 0, f'SURVIVED: {before}'
        print(f'Killed: {file}: {before}')
