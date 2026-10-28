#!/usr/bin/env python3
"""Stage 5 output directory; keep the inherited capture tool byte-identical."""
from pathlib import Path
import capture_logs

capture_logs.EVIDENCE = Path(__file__).resolve().parent.parent / 'evidence' / 'stage5'
if __name__ == '__main__':
    raise SystemExit(capture_logs.main())
