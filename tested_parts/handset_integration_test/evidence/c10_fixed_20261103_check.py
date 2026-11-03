"""Check the saved C10 hardware regression logs and Opus stream integrity."""
import importlib.util
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('streams', root/'tools/check_streams.py')
streams = importlib.util.module_from_spec(spec)
spec.loader.exec_module(streams)
results = {}
for board, peer in [(1, 2), (2, 1)]:
    path = root/f'evidence/c10_fixed_20261103_b{board}.txt'
    lines = path.read_text(errors='replace').splitlines()
    timed = []
    for line in lines:
        match = re.match(r'\s*(\d+\.\d+) (.*)', line)
        if match:
            timed.append((float(match[1]), match[2]))
    def has(start, end, text):
        return any(start <= t < end and text in msg for t, msg in timed)
    checks = {}
    for i in range(18):
        start = 4 + i * .5
        checks[f'invalid_case_{i+1}'] = has(start, start + .5,
                                         'LINK refused: invalid board ID')
    checks['no_unintended_call'] = not any(
        t < 20 and ('LINK request' in msg or 'LINK connecting' in msg or
                    'LINK connected' in msg) for t, msg in timed)
    checks['self_rejected'] = has(14, 15, f'LINK refused: no handset {board}')
    checks['slot4_rejected'] = has(15, 16, 'LINK refused: no handset 4')
    checks['slot5_rejected'] = has(16, 17, 'LINK refused: no handset 5')
    checks['offline_rejected'] = has(17, 18, 'LINK refused: 3 is offline')
    checks['invalid_during_call'] = has(27, 28, 'LINK refused: invalid board ID')
    checks['call_unaffected'] = has(28, 29, 'LINK | connected')
    checks['reverse_call_connected'] = has(39, 40, 'LINK | connected')
    checks['text_both_calls'] = all(has(t, t+1, 'TEXT from ') for t in [23, 36])
    checks['final_idle'] = has(42, 43, 'LINK | idle')
    checks['final_menu'] = has(44, 45, 'UI | screen menu')
    result = streams.check(streams.records(path, 'TX_END'),
        streams.records(root/f'evidence/c10_fixed_20261103_b{peer}.txt', 'RX_END'))
    checks['voice_intact'] = result['passed'] and len(result['streams']) == 1
    results[f'board{board}'] = {'checks': checks, 'voice': result,
                              'passed': all(checks.values())}
output = root/'evidence/c10_fixed_20261103_results.json'
output.write_text(json.dumps(results, indent=2) + '\n')
for board, result in results.items():
    print(board, 'PASS' if result['passed'] else 'FAIL',
          'failed checks:', [k for k,v in result['checks'].items() if not v])
raise SystemExit(0 if all(r['passed'] for r in results.values()) else 1)
