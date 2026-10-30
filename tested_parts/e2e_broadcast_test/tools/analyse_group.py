#!/usr/bin/env python3
"""Check board 1's encrypted plain/redundant broadcasts at boards 2 and 3."""
import argparse
import json
from pathlib import Path
import analyse_broadcast as broadcast


def analyse(logs, rate=6):
    result = broadcast.analyse(logs)
    streams = result['streams']
    checks = {}
    checks['setup'] = set(result['boards']) == {1, 2, 3} and all(
        'Build mode: encrypted group broadcast (AES-GCM), no relay' in logs.get(f'board{b}', '')
        and f'RADIO channel 11 rate {rate} ok' in logs.get(f'board{b}', '')
        for b in (1, 2, 3))
    checks['one_to_many'] = len(streams) == 2 and {
        (s['sender']['board'], s['sender']['mode']) for s in streams
    } == {(1, 'plain'), (1, 'red')} and all(
        set(map(int, s['receivers'])) == {2, 3} for s in streams)
    checks['no_security_rejects'] = all('SECURITY_REJECT' not in t for t in logs.values())
    checks['authenticated_reception'] = all(
        'SECURITY_RX ' in logs.get(f'board{b}', '') for b in (2, 3))
    checks['streams'] = bool(streams)
    for s in streams:
        tx = s['sender']
        report = tx['report'] or {}
        drops = tx['drops'] or {}
        passed = (tx['frames_logged'] == 3000 and tx['send_errors'] == 0
                  and tx['callbacks'] == 3000 and tx['callbacks_failed'] == 0
                  and report.get('sent') == 3000
                  and report.get('build_errors') == 0
                  and report.get('callback_drops') == 0
                  and report.get('unmatched') == 0
                  and report.get('heard') == 0
                  and all(drops.get(k, 1) == 0 for k in ('timer', 'radio', 'encode', 'codec', 'play_log')))
        for rx in s['receivers'].values():
            quality = rx['quality'].get('copies_in_order') or {}
            drops = rx['drops'] or {}
            passed = passed and (
                rx['primary_received'] >= 2970 and rx['crc_mismatches'] == 0
                and rx['frames_played'] == 3000 and not rx['timeout']
                and rx['report'] is not None and not rx['report'].get('abandoned', 1)
                and rx['live_late_or_missing_fraction'] is not None
                and rx['live_late_or_missing_fraction'] < .01
                and rx['live_missed'] == 0 and rx['live_matches_log']
                and rx['sources_match_firmware']
                and quality.get('correlation', 0) > .99
                and quality.get('snr_db', 0) > 20
                and all(drops.get(k, 1) == 0 for k in ('timer', 'radio', 'encode', 'codec', 'play_log')))
        s['pass'] = bool(passed)
        checks['streams'] = checks['streams'] and bool(passed)
        # The inherited estimate assumes old unicast timings; omit it here.
        s.pop('delay_estimate', None)
    result.pop('verdicts')  # The original suite expects all three senders.
    result['rate_mbps'] = rate
    result['checks'] = checks
    result['pass'] = all(checks.values())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run')
    parser.add_argument('--rate', type=int, choices=(1, 6), default=6,
                        help='PHY rate every board must report at boot (Mbps)')
    args = parser.parse_args()
    logs = {}
    for b in (1, 2, 3):
        path = Path(f'{args.run}_board{b}.txt')
        if path.exists():
            logs[f'board{b}'] = path.read_text(errors='replace')
    result = analyse(logs, args.rate)
    print(json.dumps(result, indent=2))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
