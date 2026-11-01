#!/usr/bin/env python3
"""Read-only full-install script-corpus scan and NCS<->NSS pairing ledger.

Lane s1-04-scripts. Scans the owned install (chitin.key BIFs, modules/*.rim,
rims/*.rim, patch.erf) **read-only**, deduplicates NCS payloads by SHA-256,
verifies the promoted NCS codec (byte-identical re-encode, both modes),
counts ACTION ops against the 772-entry table, pairs every unique NCS with
the shipped NSS of the same resref, merges the clean-room decompile status,
and emits the per-module action-usage table.

Nothing is written to the game directory. Payloads are hashed and decoded
in memory; only hashes, resrefs, and aggregate counts are written to the
output directory (under artifacts/, gitignored).

Stops immediately if the install executable hash drifts from the pinned
build SHA-256 0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.

CLI:
    KOTOR_GAME_DIR=/e/SteamLibrary/steamapps/common/swkotor \
      python -m tools.scripts.corpus \
        --game-dir "$KOTOR_GAME_DIR" \
        --table artifacts/action_table.json \
        --status artifacts/lanes/ncs-decompiler/cont2/out/status.jsonl \
        --out artifacts/lanes/s1-04-scripts
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time
from collections import Counter, defaultdict
from datetime import datetime, timezone

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, '..'))          # tools/
sys.path.insert(0, os.path.join(_HERE, '..', 'inventory'))

from scripts import ncs, actions, status  # noqa: E402
import kotor_inventory as ki  # noqa: E402

BUILD_SHA256 = ('0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6')
NCS_TYPE = 2010
NSS_TYPE = 2009
DEFAULT_GAME = r'E:/SteamLibrary/steamapps/common/swkotor'

_MODULE_SUFFIX = re.compile(r'_(a|adx|s)$', re.IGNORECASE)


def module_of_rim(rim_rel: str) -> str:
    """modules/end_m01aa_s.rim -> end_m01aa (module identity)."""
    stem = os.path.basename(rim_rel)[:-4] if rim_rel.lower().endswith('.rim') else rim_rel
    return _MODULE_SUFFIX.sub('', stem)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verify_install(game_dir: str) -> dict:
    """Read-only hash check of the pinned executable; stops on drift."""
    exe = os.path.join(game_dir, 'swkotor.exe')
    with open(exe, 'rb') as f:
        h = hashlib.sha256()
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    exe_sha = h.hexdigest()
    key_path = os.path.join(game_dir, 'chitin.key')
    key_sha = ki.sha256_file(key_path) if os.path.exists(key_path) else None
    return {
        'swkotor_exe': os.path.basename(exe),
        'swkotor_exe_sha256': exe_sha,
        'chitin_key_sha256': key_sha,
        'expected_build_sha256': BUILD_SHA256,
        'matches_pinned_build': exe_sha == BUILD_SHA256,
    }


# ---------------------------------------------------------------- sources
def _bif_path(game_dir: str, name: str) -> str:
    rel = name.replace('\\', '/')
    if rel.lower().startswith('data/'):
        return os.path.join(game_dir, rel)
    return os.path.join(game_dir, 'data', os.path.basename(rel))


def iter_bif_scripts(game_dir: str):
    """Yield script occurrences from chitin.key -> BIFs (base layer)."""
    key = ki.parse_key(os.path.join(game_dir, 'chitin.key'))
    bifs = key['bifs']
    wanted = [e for e in key['entries'] if e['type'] in (NCS_TYPE, NSS_TYPE)]
    cache: dict[str, dict] = {}
    for e in wanted:
        bmeta = bifs[e['bif']]
        path = _bif_path(game_dir, bmeta['name'])
        if path not in cache:
            bif = ki.parse_bif(path)
            table = {}
            for row in bif['fixed'] + bif['var']:
                table.setdefault(row['id'] & 0xFFFFF, row)
            cache[path] = table
        row = cache[path].get(e['id'] & 0xFFFFF)
        source = 'bif:' + bmeta['name'].replace('\\', '/')
        if row is None:
            yield {'source_kind': 'bif', 'source': source, 'resref': e['resref'],
                   'rtype': e['type'], 'rid': e['id'], 'offset': None, 'size': None,
                   'payload': None, 'error': 'key entry not found in BIF table'}
            continue
        with open(path, 'rb') as f:
            f.seek(row['offset'])
            payload = f.read(row['size'])
        yield {'source_kind': 'bif', 'source': source, 'resref': e['resref'],
               'rtype': e['type'], 'rid': e['id'], 'offset': row['offset'],
               'size': row['size'], 'payload': payload}


def iter_rim_scripts(game_dir: str, rel_dir: str):
    """Yield script occurrences from every RIM under ``rel_dir`` (modules/ or rims/)."""
    d = os.path.join(game_dir, rel_dir)
    if not os.path.isdir(d):
        return
    for fn in sorted(os.listdir(d)):
        if not fn.lower().endswith('.rim'):
            continue
        rel_path = rel_dir.replace('\\', '/') + '/' + fn
        rim = ki.parse_rim(os.path.join(game_dir, rel_path))
        if not rim:
            continue
        with open(os.path.join(game_dir, rel_path), 'rb') as f:
            for e in rim['entries']:
                if e['type'] not in (NCS_TYPE, NSS_TYPE) or not e['size']:
                    continue
                f.seek(e['offset'])
                payload = f.read(e['size'])
                yield {'source_kind': 'rim', 'source': 'rim:' + rel_path,
                       'resref': e['resref'], 'rtype': e['type'], 'rid': e['id'],
                       'offset': e['offset'], 'size': e['size'], 'payload': payload}


def iter_erf_scripts(game_dir: str, rel_path: str):
    """Yield script occurrences from an ERF/MOD (patch.erf)."""
    path = os.path.join(game_dir, rel_path)
    if not os.path.exists(path):
        return
    erf = ki.parse_erf(path)
    if not erf:
        return
    with open(path, 'rb') as f:
        for e in erf['entries']:
            if e['type'] not in (NCS_TYPE, NSS_TYPE) or not e['size']:
                continue
            f.seek(e['offset'])
            payload = f.read(e['size'])
            yield {'source_kind': 'erf', 'source': 'erf:' + rel_path.replace('\\', '/'),
                   'resref': e['resref'], 'rtype': e['type'], 'rid': e['id'],
                   'offset': e['offset'], 'size': e['size'], 'payload': payload}


def iter_all_scripts(game_dir: str):
    yield from iter_bif_scripts(game_dir)
    yield from iter_rim_scripts(game_dir, 'modules')
    yield from iter_rim_scripts(game_dir, 'rims')
    yield from iter_erf_scripts(game_dir, 'patch.erf')


# ---------------------------------------------------------------- decode pool
_ARITY = {}


def _worker_init(arity_compact):
    global _ARITY
    _ARITY = arity_compact


def _worker_check(item):
    sha, payload = item
    rec = ncs.check_bytes(payload)
    rec['sha256'] = sha
    if not rec.get('ok'):
        return rec
    anomalies = []
    for ins in ncs.decode(payload):
        a = ins.action
        if a is None:
            continue
        aid, argc = a
        ar = _ARITY.get(aid)
        if ar is None:
            anomalies.append({'action': aid, 'argc': argc, 'kind': 'unknown_id'})
        elif argc < ar['required'] or argc > ar['total']:
            anomalies.append({'action': aid, 'name': ar['name'], 'argc': argc,
                              'declared': ar['total'], 'required': ar['required'],
                              'kind': 'action_missing_args' if argc < ar['required']
                              else 'action_extra_args'})
    rec['arity_anomalies'] = anomalies
    return rec


def decode_unique(unique_payloads: dict[str, bytes], arity_compact: dict, jobs: int) -> dict[str, dict]:
    """Decode + re-encode + action-scan every unique payload (multiprocessed)."""
    from concurrent.futures import ProcessPoolExecutor
    items = list(unique_payloads.items())
    if jobs > 1 and len(items) > 64:
        with ProcessPoolExecutor(max_workers=jobs, initializer=_worker_init,
                                 initargs=(arity_compact,)) as ex:
            results = list(ex.map(_worker_check, items, chunksize=32))
    else:
        _worker_init(arity_compact)
        results = [_worker_check(it) for it in items]
    return {r['sha256']: r for r in results}


# ---------------------------------------------------------------- ledger
def build_ledger(game_dir: str, table_path: str, status_by_sha: dict | None,
                 jobs: int) -> dict:
    """Scan ``game_dir`` read-only and build the full ledger dict (no writes)."""
    t0 = time.time()
    table = actions.load_action_table(table_path)
    if table['build_sha256'] and table['build_sha256'] != BUILD_SHA256:
        raise SystemExit('action table build %s != pinned build' % table['build_sha256'])
    arity_compact = {aid: {'name': a['name'], 'total': a['total'], 'required': a['required']}
                     for aid, a in table['arity'].items()}
    if status_by_sha is None:
        status_by_sha = {}

    # ---- pass 1: occurrences -------------------------------------------------
    ncs_occ: list[dict] = []
    nss_occ: list[dict] = []
    errors: list[dict] = []
    for r in iter_all_scripts(game_dir):
        if r.get('error') or r.get('payload') is None:
            errors.append({k: v for k, v in r.items() if k != 'payload'})
            continue
        r['sha256'] = sha256_bytes(r['payload'])
        (ncs_occ if r['rtype'] == NCS_TYPE else nss_occ).append(r)

    # unique NCS by sha256
    unique: dict[str, dict] = {}
    for r in ncs_occ:
        u = unique.setdefault(r['sha256'], {
            'sha256': r['sha256'], 'size': r['size'], 'occurrences': 0,
            'resrefs': set(), 'sources': set(), 'modules': set(),
            'rim_sources': set(), 'payload': r['payload'],
        })
        u['occurrences'] += 1
        u['resrefs'].add(r['resref'])
        u['sources'].add(r['source'])
        if r['source_kind'] == 'rim':
            u['rim_sources'].add(r['source'])
            if r['source'].startswith('rim:modules/'):
                u['modules'].add(module_of_rim(r['source'][len('rim:'):]))
        r.pop('payload', None)
    unique_payloads = {sha: u.pop('payload') for sha, u in unique.items()}

    # NSS by lowercase resref
    nss_by_resref: dict[str, list[dict]] = defaultdict(list)
    for r in nss_occ:
        nss_by_resref[r['resref'].lower()].append({
            'resref': r['resref'], 'sha256': r['sha256'], 'size': r['size'],
            'source': r['source'], 'source_kind': r['source_kind'],
        })

    # ---- pass 2: decode + verify unique corpus -------------------------------
    decoded = decode_unique(unique_payloads, arity_compact, jobs)

    # per-module action usage: module RIM -> {sha: count} then merge decoded
    rim_sha_counts: dict[str, Counter] = defaultdict(Counter)
    for r in ncs_occ:
        if r['source'].startswith('rim:modules/'):
            rim_sha_counts[r['source']][r['sha256']] += 1
    module_usage: dict[str, dict] = {}
    for rim_src, counts in sorted(rim_sha_counts.items()):
        rim_rel = rim_src[len('rim:'):]
        agg_actions: Counter = Counter()
        ops = 0
        for sha, cnt in counts.items():
            d = decoded.get(sha)
            if not d or not d.get('ok'):
                continue
            agg_actions.update({aid: c * cnt for aid, c in d['actions'].items()})
            ops += d['action_ops'] * cnt
        module_usage[rim_rel] = {
            'module': module_of_rim(rim_rel),
            'action_ops': ops,
            'distinct_actions': len(agg_actions),
            'actions': {str(aid): {'name': table['arity'][aid]['name'] if aid in table['arity'] else None,
                                   'occurrences': c}
                        for aid, c in sorted(agg_actions.items())},
        }

    # ---- assemble ledger ------------------------------------------------------
    verification = {
        'ncs_occurrences': len(ncs_occ),
        'ncs_unique_sha256': len(unique),
        'ncs_duplicate_occurrences': len(ncs_occ) - len(unique),
        'ncs_bytes_occurrences': sum(r['size'] for r in ncs_occ),
        'ncs_bytes_unique': sum(u['size'] for u in unique.values()),
        'decode_failures': sum(1 for d in decoded.values() if not d.get('ok')),
        'reencode_readback_identical': sum(1 for d in decoded.values() if d.get('readback_identical')),
        'reencode_semantic_identical': sum(1 for d in decoded.values() if d.get('semantic_identical')),
        'total_instructions': sum(d.get('instructions', 0) for d in decoded.values()),
        'total_action_ops': sum(d.get('action_ops', 0) for d in decoded.values()),
        'unknown_action_ops': sum(d.get('unknown_action_ops', sum(
            c for aid, c in d.get('actions', {}).items() if aid not in table['arity']))
            for d in decoded.values() if d.get('ok')),
        'distinct_actions_used': len({aid for d in decoded.values() if d.get('ok')
                                      for aid in d.get('actions', {})}),
        'arity_anomaly_files': 0,
        'arity_anomaly_ops': [],
    }
    anomaly_rows = []
    scripts = []
    for sha in sorted(unique):
        u = unique[sha]
        d = decoded.get(sha, {'ok': False, 'error': 'not decoded'})
        nss_hits = []
        for resref in sorted(u['resrefs'], key=str.lower):
            nss_hits.extend(nss_by_resref.get(resref.lower(), []))
        # dedupe nss hits by (resref, sha256, source)
        seen_nss = set()
        nss_list = []
        for h in nss_hits:
            k = (h['resref'].lower(), h['sha256'], h['source'])
            if k not in seen_nss:
                seen_nss.add(k)
                nss_list.append(h)
        nss_base = [h for h in nss_list if h['source_kind'] == 'bif']
        anom = d.get('arity_anomalies', []) if d.get('ok') else []
        if anom:
            verification['arity_anomaly_files'] += 1
            verification['arity_anomaly_ops'].extend(
                [{'resref': r, 'sha256': sha, **a} for a in anom for r in sorted(u['resrefs'], key=str.lower)])
            anomaly_rows.append({'sha256': sha, 'resref': sorted(u['resrefs'], key=str.lower)[0],
                                 'arity_anomalies': anom})
        st = status_by_sha.get(sha)
        scripts.append({
            'sha256': sha,
            'size': u['size'],
            'occurrences': u['occurrences'],
            'resrefs': sorted(u['resrefs'], key=str.lower),
            'sources': sorted(u['sources']),
            'modules': sorted(u['modules']),
            'nss': nss_list,
            'nss_base': nss_base,
            'nss_present': bool(nss_list),
            'nss_base_present': bool(nss_base),
            'decode_ok': d.get('ok', False),
            'instructions': d.get('instructions', 0),
            'reencode_readback_identical': d.get('readback_identical'),
            'reencode_semantic_identical': d.get('semantic_identical'),
            'action_ops': d.get('action_ops', 0),
            'action_ids': d.get('action_ids', []),
            'arity_anomalies': anom,
            'decompile': st,
        })

    # pairing coverage
    paired = sum(1 for s in scripts if s['nss_present'])
    paired_base = sum(1 for s in scripts if s['nss_base_present'])
    base_ncs = [r for r in ncs_occ if r['source_kind'] == 'bif']
    base_sha = {r['sha256'] for r in base_ncs}
    base_paired = sum(1 for s in scripts if s['sha256'] in base_sha and s['nss_base_present'])
    nss_resrefs = set(nss_by_resref)
    pairing = {
        'nss_occurrences': len(nss_occ),
        'nss_unique_sha256': len({r['sha256'] for r in nss_occ}),
        'nss_base_occurrences': sum(1 for r in nss_occ if r['source_kind'] == 'bif'),
        'nss_by_source': dict(sorted(Counter(r['source'] for r in nss_occ).items())),
        'base_bif_ncs_occurrences': len(base_ncs),
        'unique_ncs_from_base_bif': len(base_sha),
        'unique_ncs_with_nss_resref': paired,
        'unique_ncs_with_base_nss_resref': paired_base,
        'unique_ncs_without_nss': len(unique) - paired,
        'pairing_coverage_pct': round(100.0 * paired / len(unique), 2) if unique else 0.0,
        'base_pairing_coverage_pct': round(100.0 * base_paired / len(base_sha), 2) if base_sha else 0.0,
    }
    # nss resrefs that never match any NCS resref (source-only)
    ncs_resref_set = {r.lower() for s in scripts for r in s['resrefs']}
    pairing['nss_resrefs_without_any_ncs'] = sorted(nss_resrefs - ncs_resref_set)

    decomp_summary = status.summarize(status_by_sha) if status_by_sha else None
    if decomp_summary:
        matched = sum(1 for s in scripts if s['decompile'])
        decomp_summary['matched_in_corpus'] = matched

    report = {
        'tool': 'tools/scripts/corpus.py',
        'generated_utc': datetime.now(timezone.utc).isoformat(),
        'game_dir': os.path.abspath(game_dir),
        'build_sha256': BUILD_SHA256,
        'action_table': {'path': os.path.abspath(table_path),
                         'build_sha256': table['build_sha256'],
                         'actions': len(table['arity'])},
        'verification': verification,
        'pairing': pairing,
        'decompile_status': decomp_summary,
        'bug_parity_arity_anomalies': status.bug_parity_items(anomaly_rows),
        'module_action_usage': module_usage,
        'scripts': scripts,
        'errors': errors,
        'elapsed_seconds': round(time.time() - t0, 1),
    }
    return report


# ---------------------------------------------------------------- main scan
def scan(game_dir: str, table_path: str, status_path: str | None,
         out_dir: str, jobs: int) -> dict:
    """Verify the pinned install read-only, build the ledger, write artifacts."""
    install = verify_install(game_dir)
    if not install['matches_pinned_build']:
        raise SystemExit('install drift: swkotor.exe sha256 %s != pinned %s; stopping'
                         % (install['swkotor_exe_sha256'], BUILD_SHA256))
    status_by_sha = (status.load_status_jsonl(status_path)
                     if status_path and os.path.exists(status_path) else {})
    report = build_ledger(game_dir, table_path, status_by_sha, jobs)
    report['install_check'] = install

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'corpus.json'), 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=1, sort_keys=True)
    # per-module action usage as its own artifact + CSV
    module_usage = report['module_action_usage']
    mau = {
        'tool': 'tools/scripts/corpus.py',
        'build_sha256': BUILD_SHA256,
        'modules': module_usage,
        'totals': {
            'module_rims': len(module_usage),
            'action_ops_in_module_rims': sum(m['action_ops'] for m in module_usage.values()),
        },
    }
    with open(os.path.join(out_dir, 'module_action_usage.json'), 'w', encoding='utf-8') as f:
        json.dump(mau, f, indent=1, sort_keys=True)
    with open(os.path.join(out_dir, 'module_action_usage.csv'), 'w', encoding='utf-8') as f:
        f.write('module_rim,module,action_id,action_name,occurrences\n')
        for rim_rel, m in sorted(mau['modules'].items()):
            for aid, info in m['actions'].items():
                f.write('%s,%s,%s,"%s",%d\n' % (rim_rel, m['module'], aid,
                                                 (info['name'] or '').replace('"', '""'),
                                                 info['occurrences']))
    with open(os.path.join(out_dir, 'verification.json'), 'w', encoding='utf-8') as f:
        json.dump({k: report[k] for k in ('verification', 'pairing', 'decompile_status',
                                          'bug_parity_arity_anomalies', 'install_check')},
                  f, indent=1, sort_keys=True)
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--game-dir', default=os.environ.get('KOTOR_GAME_DIR', DEFAULT_GAME))
    ap.add_argument('--table', default='artifacts/action_table.json')
    ap.add_argument('--status', default='artifacts/lanes/ncs-decompiler/cont2/out/status.jsonl')
    ap.add_argument('--out', default='artifacts/lanes/s1-04-scripts')
    ap.add_argument('--jobs', type=int, default=min(16, (os.cpu_count() or 4)))
    args = ap.parse_args()
    report = scan(args.game_dir, args.table, args.status, args.out, args.jobs)
    v = report['verification']
    p = report['pairing']
    print(json.dumps({
        'install': report['install_check'],
        'verification': {k: v[k] for k in ('ncs_occurrences', 'ncs_unique_sha256',
                                           'decode_failures', 'reencode_readback_identical',
                                           'reencode_semantic_identical', 'total_instructions',
                                           'total_action_ops', 'unknown_action_ops',
                                           'distinct_actions_used', 'arity_anomaly_files')},
        'pairing': {k: p[k] for k in ('nss_occurrences', 'nss_unique_sha256',
                                      'base_bif_ncs_occurrences', 'unique_ncs_from_base_bif',
                                      'unique_ncs_with_nss_resref', 'unique_ncs_without_nss',
                                      'pairing_coverage_pct', 'base_pairing_coverage_pct')},
        'decompile_status': report['decompile_status'],
        'module_rims': len(report['module_action_usage']),
        'errors': len(report['errors']),
        'out': os.path.abspath(args.out),
    }, indent=1, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
