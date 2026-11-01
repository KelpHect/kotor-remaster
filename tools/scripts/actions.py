#!/usr/bin/env python3
"""Action-table access and per-file / per-module action usage.

The 772-entry action table is *data* produced by
``tools/analysis/extract_actions.py`` (nwscript.nss prototype order +
``InitializeCommands`` table writes at 0x0054c960 / 0x005cd2c0, dispatcher
0x0052c0d0) for build SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
This module never reads the game; pass the table path explicitly.

Verified on the shipped corpus: 285,015 ACTION instructions over the 8,666
unique NCS, 0 unknown ids (all within 0..771); see docs/recon/20_SCRIPT_CORPUS.md.

CLI:
    python -m tools.scripts.actions --table artifacts/action_table.json --summary
    python -m tools.scripts.actions --table artifacts/action_table.json \
        --ncs-dir <dir-of-unique-.ncs> --out action_usage.json
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, '..'))
from scripts import ncs  # noqa: E402

ACTION_COUNT = 772


def split_params(s: str) -> list[str]:
    """Split a C-style parameter list on top-level commas."""
    out, cur, depth = [], [], 0
    for ch in s:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if ''.join(cur).strip():
        out.append(''.join(cur).strip())
    return out


def load_action_table(path: str) -> dict:
    """Load the extract_actions.py JSON; returns {'actions': [...], 'arity': {id: {...}}}."""
    with open(path, encoding='utf-8') as f:
        d = json.load(f)
    actions = d['actions']
    if len(actions) != ACTION_COUNT:
        raise ValueError('action table has %d entries, expected %d' % (len(actions), ACTION_COUNT))
    arity = {}
    for a in actions:
        params = split_params(a['args']) if (a['args'] or '').strip() else []
        arity[a['id']] = {
            'name': a['name'],
            'args': a['args'],
            'handler': a['handler'],
            'total': len(params),
            'required': sum(1 for p in params if '=' not in p),
        }
    return {'build_sha256': d.get('build_sha256'), 'dispatch': d.get('dispatch'),
            'actions': actions, 'arity': arity}


def arity_violation(table: dict, action_id: int, argc: int) -> str | None:
    """Return a violation tag when argc is outside [required, total].

    Arguments are pushed right-to-left, so ``args[0]`` is the last push.
    argc > total means extra deepest pushes (inert unless the engine reads
    them); argc < required means a missing slot (undefined/stale read).
    """
    a = table['arity'].get(action_id)
    if a is None:
        return 'unknown_id'
    if argc < a['required']:
        return 'action_missing_args'
    if argc > a['total']:
        return 'action_extra_args'
    return None


def action_usage_bytes(data: bytes) -> dict:
    """Decode one NCS payload; return per-action occurrence counts + anomalies."""
    usage: Counter = Counter()
    argc_hist: dict[int, Counter] = defaultdict(Counter)
    anomalies = []
    for off, aid, argc in ncs.iter_actions(data):
        usage[aid] += 1
        argc_hist[aid][argc] += 1
    return {'actions': dict(usage), 'argc': {k: dict(v) for k, v in argc_hist.items()}}


def scan_usage(table: dict, sha: str, data: bytes, resref: str = '') -> dict:
    """Per-file action usage plus arity anomalies against the 772-entry table."""
    u = action_usage_bytes(data)
    anomalies = []
    for aid, cnt in sorted(u['actions'].items()):
        if aid not in table['arity']:
            anomalies.append({'action': aid, 'argc': None, 'kind': 'unknown_id',
                              'occurrences': cnt})
            continue
        for argc, acnt in sorted(u['argc'].get(aid, {}).items()):
            v = arity_violation(table, aid, argc)
            if v:
                anomalies.append({'action': aid, 'name': table['arity'][aid]['name'],
                                  'argc': argc, 'declared': table['arity'][aid]['total'],
                                  'required': table['arity'][aid]['required'],
                                  'kind': v, 'occurrences': acnt})
    unknown = sum(c for aid, c in u['actions'].items() if aid not in table['arity'])
    return {
        'sha256': sha,
        'resref': resref,
        'action_ops': sum(u['actions'].values()),
        'distinct_actions': len(u['actions']),
        'action_ids': sorted(u['actions']),
        'actions': u['actions'],
        'unknown_action_ops': unknown,
        'arity_anomalies': anomalies,
    }


def aggregate_module_usage(per_file: list[dict]) -> dict:
    """Aggregate per-file usage rows into one module-level usage dict."""
    actions: Counter = Counter()
    files: set[str] = set()
    ops = 0
    for row in per_file:
        actions.update(row['actions'])
        files.add(row['sha256'])
        ops += row['action_ops']
    return {'action_ops': ops, 'distinct_actions': len(actions),
            'unique_files': len(files), 'actions': dict(actions)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--table', default='artifacts/action_table.json')
    ap.add_argument('--summary', action='store_true', help='print table summary and exit')
    ap.add_argument('--ncs-dir', help='directory of unique .ncs files to scan')
    ap.add_argument('--out', help='JSON output for --ncs-dir scan')
    args = ap.parse_args()
    table = load_action_table(args.table)
    if args.summary:
        named = sum(1 for a in table['arity'].values() if a['name'])
        handled = sum(1 for a in table['arity'].values() if a['handler'])
        print(json.dumps({'build_sha256': table['build_sha256'],
                          'actions': len(table['arity']), 'named': named,
                          'with_handler': handled,
                          'dispatch': table['dispatch']}, indent=2))
        return 0
    if not args.ncs_dir:
        ap.error('--ncs-dir or --summary required')
    per_file = []
    unknown_total = 0
    anomaly_files = []
    for fn in sorted(os.listdir(args.ncs_dir)):
        if not fn.lower().endswith('.ncs'):
            continue
        with open(os.path.join(args.ncs_dir, fn), 'rb') as f:
            data = f.read()
        import hashlib
        sha = hashlib.sha256(data).hexdigest()
        resref = fn.rsplit('__', 1)[0] if '__' in fn else fn[:-4]
        row = scan_usage(table, sha, data, resref)
        per_file.append(row)
        unknown_total += row['unknown_action_ops']
        if row['arity_anomalies']:
            anomaly_files.append(row)
    used = set()
    ops = 0
    for row in per_file:
        used.update(row['action_ids'])
        ops += row['action_ops']
    summary = {
        'tool': 'tools/scripts/actions.py',
        'table': os.path.abspath(args.table),
        'build_sha256': table['build_sha256'],
        'files': len(per_file),
        'total_action_ops': ops,
        'unknown_action_ops': unknown_total,
        'actions_used': len(used),
        'actions_unused': sorted(set(range(ACTION_COUNT)) - used),
        'arity_anomaly_files': [
            {'sha256': r['sha256'], 'resref': r['resref'],
             'anomalies': r['arity_anomalies']} for r in anomaly_files],
    }
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, 'w', encoding='utf-8') as f:
            json.dump({'summary': summary, 'per_file': per_file}, f, indent=2, sort_keys=True)
    print(json.dumps({k: v for k, v in summary.items()
                      if k not in ('actions_unused', 'arity_anomaly_files')}, indent=2))
    print('unused actions:', len(summary['actions_unused']),
          'arity-anomaly files:', len(anomaly_files))
    return 0 if unknown_total == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
