#!/usr/bin/env python3
"""Per-file decompile-status classes and bug-parity anomaly listing.

Status classes follow docs/recon/09_SCRIPTS.md (clean-room decompiler,
``artifacts/lanes/ncs-decompiler/cont2``, final-v10 run):

    exact            byte-identical NCS + full instruction stream (8,428 = 97.26%)
    compiler-limited compiled, stream differs; cannot be byte-exact with the
                     shipped compiler (228, incl. 3 body-order layout files)
    header-limited   recompile fails only against the full 12-prototype header;
                     byte-identical with the matching lane-local prototype
                     header (5 -- exactly the 5 action-arity anomaly files)
    reachable-only   differs only in code unreachable from the entry/STORESTATE
                     continuations; not full-stream-equivalent (5)

The status JSONL is produced by the lane-local clean-room decompiler and is
an artifact (gitignored); this module only reads it and merges by sha256.

CLI:
    python -m tools.scripts.status --status artifacts/lanes/ncs-decompiler/cont2/out/status.jsonl \
        --out status_summary.json
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter

CLASS_EXACT = 'exact'
CLASS_COMPILER_LIMITED = 'compiler-limited'
CLASS_HEADER_LIMITED = 'header-limited'
CLASS_REACHABLE_ONLY = 'reachable-only'

CLASS_ORDER = [CLASS_EXACT, CLASS_COMPILER_LIMITED, CLASS_HEADER_LIMITED, CLASS_REACHABLE_ONLY]

# raw (comparison_status, status) -> class, per recon/09 residual taxonomy
_RAW_MAP = {
    ('exact', 'exact'): CLASS_EXACT,
    ('mismatch', 'mismatch'): CLASS_COMPILER_LIMITED,
    ('reachable', 'reachable'): CLASS_REACHABLE_ONLY,
    (None, 'compile_fail'): CLASS_HEADER_LIMITED,
}


def classify_row(row: dict) -> str:
    """Map one decompiler status row to one of the four published classes."""
    key = (row.get('comparison_status'), row.get('status'))
    if key in _RAW_MAP:
        return _RAW_MAP[key]
    # tolerate rows that only carry comparison_status
    cs = row.get('comparison_status')
    if cs == 'exact':
        return CLASS_EXACT
    if cs == 'mismatch':
        return CLASS_COMPILER_LIMITED
    if cs == 'reachable':
        return CLASS_REACHABLE_ONLY
    if row.get('status') == 'compile_fail' or not row.get('compiled', True):
        return CLASS_HEADER_LIMITED
    raise ValueError('unclassifiable status row: comparison_status=%r status=%r'
                     % (row.get('comparison_status'), row.get('status')))


def load_status_jsonl(path: str) -> dict[str, dict]:
    """Load a decompiler status JSONL keyed by input_sha256."""
    out = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            sha = row.get('input_sha256')
            if not sha:
                continue
            out[sha] = {
                'class': classify_row(row),
                'comparison_status': row.get('comparison_status'),
                'raw_status': row.get('status'),
                'byte_equal': row.get('byte_equal'),
                'full_stream_equal': row.get('full_stream_equal'),
                'instructions': row.get('instructions'),
                'name': row.get('name'),
                'notes': row.get('notes'),
            }
    return out


def summarize(status_by_sha: dict[str, dict]) -> dict:
    counts = Counter(v['class'] for v in status_by_sha.values())
    total = len(status_by_sha)
    return {
        'files': total,
        'classes': {c: {'count': counts.get(c, 0),
                        'share_pct': round(100.0 * counts.get(c, 0) / total, 2) if total else 0.0}
                    for c in CLASS_ORDER},
        'header_limited_files': sorted(v['name'] for v in status_by_sha.values()
                                       if v['class'] == CLASS_HEADER_LIMITED),
    }


def bug_parity_items(arity_anomaly_rows: list[dict]) -> list[dict]:
    """Flatten actions.py arity-anomaly rows into bug-parity ledger entries.

    Each entry: resref, sha256, action id/name, argc seen, declared arity,
    kind (action_missing_args / action_extra_args), and the recon/09 finding.
    """
    items = []
    for row in arity_anomaly_rows:
        for a in row.get('arity_anomalies', []):
            items.append({
                'resref': row.get('resref'),
                'sha256': row.get('sha256'),
                'action_id': a.get('action'),
                'action_name': a.get('name'),
                'argc_seen': a.get('argc'),
                'declared_total': a.get('declared'),
                'declared_required': a.get('required'),
                'kind': a.get('kind'),
            })
    items.sort(key=lambda x: (x['resref'] or '', x['action_id'] or -1))
    return items


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--status', required=True, help='decompiler status JSONL (input_sha256 keyed)')
    ap.add_argument('--out', help='JSON summary output path')
    args = ap.parse_args()
    status = load_status_jsonl(args.status)
    summary = summarize(status)
    summary['tool'] = 'tools/scripts/status.py'
    summary['status_source'] = os.path.abspath(args.status)
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, 'w', encoding='utf-8') as f:
            json.dump(summary, f, indent=2, sort_keys=True)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
