#!/usr/bin/env python3
"""Synthetic-install tests for tools/scripts/corpus.py.

Builds a tiny fake install (KEY+BIF, module RIM, global RIM, patch ERF) with
the deterministic fixture writers from tools/resources/kotor_archive.py --
no game data -- and runs the full ledger build on it. Run from the repo root:

    python -m unittest discover -s tools/scripts/tests -v
"""
from __future__ import annotations

import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, '..', '..'))                 # tools/
sys.path.insert(0, os.path.join(_HERE, '..', '..', 'resources'))     # tools/resources

from scripts import ncs, actions, corpus  # noqa: E402
import kotor_archive as ka  # noqa: E402


def ncs_payload(*action_pairs, tag=''):
    """Distinct valid NCS payloads; ``tag`` makes payloads unique via CONST string."""
    insns = []
    if tag:
        raw = bytes([ncs.CONST_STRING]) + struct.pack('>H', len(tag)) + tag.encode()
        insns.append(ncs.Instruction(offset=0, opcode=0x04, name='CONST', shape='const',
                                     qualifier=ncs.CONST_STRING, ops=[('b', tag.encode())],
                                     raw=raw))
    for aid, argc in action_pairs:
        insns.append(ncs.Instruction(offset=0, opcode=0x05, name='ACTION', shape='action',
                                     qualifier=0x20,
                                     ops=[('c', struct.pack('>HB', aid, argc))],
                                     raw=bytes([0x20]) + struct.pack('>HB', aid, argc)))
    insns.append(ncs.Instruction(offset=0, opcode=0x20, name='RETN', shape='qual',
                                 qualifier=0x03, ops=[], raw=b'\x03'))
    return ncs.encode(insns, 'readback')


def make_table(path):
    acts = []
    for i in range(actions.ACTION_COUNT):
        args = {6: 'object o, action a', 7: 'float f, int i', 574: 'int n, object o'}.get(i, '')
        acts.append({'id': i, 'name': 'Act%03d' % i, 'args': args, 'handler': 'H_%x' % i})
    with open(path, 'w', encoding='utf-8') as f:
        json.dump({'build_sha256': None, 'actions': acts}, f)
    return path


def build_fake_game(root):
    """Synthetic install: 6 unique NCS across BIF/module RIM/global RIM/ERF."""
    os.makedirs(os.path.join(root, 'data'), exist_ok=True)
    os.makedirs(os.path.join(root, 'modules'), exist_ok=True)
    os.makedirs(os.path.join(root, 'rims'), exist_ok=True)
    with open(os.path.join(root, 'swkotor.exe'), 'wb') as f:
        f.write(b'FAKE EXE -- not the pinned build')

    p1 = ncs_payload((6, 2), tag='p1')       # a_main (paired, in bif+module+global)
    p2 = ncs_payload((7, 2), tag='p2')       # b_util (paired, base only)
    p3 = ncs_payload((6, 2), (7, 2), tag='p3')  # c_nosrc (no NSS)
    p4 = ncs_payload((574, 1), tag='p4')     # m_only (module only, arity anomaly)
    p5 = ncs_payload((6, 2), tag='p5')       # g_glob (global rim only)
    p6 = ncs_payload((7, 2), tag='p6')       # p_pat (patch.erf only)

    ka.write_key_bif(root, [
        ('a_main', 2010, p1), ('a_main', 2009, b'void main() { AssignCommand(GetFirstPC(), ActionRandom()); }\n'),
        ('b_util', 2010, p2), ('b_util', 2009, b'void main() {}\n'),
        ('c_nosrc', 2010, p3),
    ], bif_rel='data\\scripts.bif')
    ka.write_rim(os.path.join(root, 'modules', 'mod01_s.rim'),
                 [('a_main', 2010, p1), ('m_only', 2010, p4)])
    ka.write_rim(os.path.join(root, 'modules', 'mod01.rim'),
                 [('mod_area', 2022, b'fake are')])
    ka.write_rim(os.path.join(root, 'rims', 'global.rim'),
                 [('a_main', 2010, p1), ('g_glob', 2010, p5)])
    ka.write_erf(os.path.join(root, 'patch.erf'), [('p_pat', 2010, p6)])
    return {'a_main': p1, 'b_util': p2, 'c_nosrc': p3, 'm_only': p4, 'g_glob': p5, 'p_pat': p6}


class TestHelpers(unittest.TestCase):
    def test_module_of_rim(self):
        self.assertEqual(corpus.module_of_rim('modules/end_m01aa_s.rim'), 'end_m01aa')
        self.assertEqual(corpus.module_of_rim('modules/danm13.rim'), 'danm13')
        self.assertEqual(corpus.module_of_rim('modules/STUNT_19_s.rim'), 'STUNT_19')
        self.assertEqual(corpus.module_of_rim('rims/global.rim'), 'global')

    def test_verify_install_detects_drift(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, 'swkotor.exe'), 'wb') as f:
                f.write(b'not the real exe')
            with open(os.path.join(d, 'chitin.key'), 'wb') as f:
                f.write(b'fake key')
            info = corpus.verify_install(d)
        self.assertFalse(info['matches_pinned_build'])
        self.assertEqual(info['expected_build_sha256'], corpus.BUILD_SHA256)
        self.assertEqual(info['swkotor_exe_sha256'],
                         hashlib.sha256(b'not the real exe').hexdigest())

    def test_scan_stops_on_drift(self):
        with tempfile.TemporaryDirectory() as d:
            build_fake_game(d)
            table = make_table(os.path.join(d, 'table.json'))
            with self.assertRaises(SystemExit) as cm:
                corpus.scan(d, table, None, os.path.join(d, 'out'), jobs=1)
            self.assertIn('install drift', str(cm.exception))


class TestLedger(unittest.TestCase):
    def test_build_ledger_end_to_end(self):
        with tempfile.TemporaryDirectory() as d:
            payloads = build_fake_game(d)
            table = make_table(os.path.join(d, 'table.json'))
            shas = {k: hashlib.sha256(v).hexdigest() for k, v in payloads.items()}
            status_by_sha = {shas['a_main']: {'class': 'exact', 'name': 'a_main',
                                              'comparison_status': 'exact'}}
            report = corpus.build_ledger(d, table, status_by_sha, jobs=1)

            v = report['verification']
            self.assertEqual(v['ncs_occurrences'], 8)          # 3+1+1+1+1+1
            self.assertEqual(v['ncs_unique_sha256'], 6)
            self.assertEqual(v['ncs_duplicate_occurrences'], 2)
            self.assertEqual(v['decode_failures'], 0)
            self.assertEqual(v['reencode_readback_identical'], 6)
            self.assertEqual(v['reencode_semantic_identical'], 6)
            self.assertEqual(v['total_action_ops'], 7)        # unique: 1+1+2+1+1+1
            self.assertEqual(v['unknown_action_ops'], 0)
            self.assertEqual(v['distinct_actions_used'], 3)   # 6, 7, 574
            self.assertEqual(v['arity_anomaly_files'], 1)     # m_only: 574 argc 1 < 2

            p = report['pairing']
            self.assertEqual(p['unique_ncs_with_nss_resref'], 2)
            self.assertEqual(p['unique_ncs_without_nss'], 4)
            self.assertEqual(p['pairing_coverage_pct'], round(100.0 * 2 / 6, 2))
            self.assertEqual(p['nss_occurrences'], 2)
            self.assertEqual(p['base_bif_ncs_occurrences'], 3)
            self.assertEqual(p['nss_resrefs_without_any_ncs'], [])  # a_main/b_util match NCS

            by_sha = {s['sha256']: s for s in report['scripts']}
            a_main = by_sha[shas['a_main']]
            self.assertEqual(a_main['occurrences'], 3)
            self.assertEqual(sorted(a_main['resrefs']), ['a_main'])
            self.assertEqual(sorted(a_main['modules']), ['mod01'])
            self.assertIn('bif:data/scripts.bif', a_main['sources'])
            self.assertIn('rim:modules/mod01_s.rim', a_main['sources'])
            self.assertIn('rim:rims/global.rim', a_main['sources'])
            self.assertTrue(a_main['nss_present'])
            self.assertEqual(a_main['nss'][0]['resref'], 'a_main')
            self.assertEqual(a_main['nss'][0]['sha256'],
                             hashlib.sha256(b'void main() { AssignCommand(GetFirstPC(), ActionRandom()); }\n').hexdigest())
            self.assertEqual(a_main['decompile']['class'], 'exact')
            m_only = by_sha[shas['m_only']]
            self.assertEqual(m_only['arity_anomalies'][0]['kind'], 'action_missing_args')
            self.assertEqual(m_only['arity_anomalies'][0]['action'], 574)

            mau = report['module_action_usage']
            self.assertIn('modules/mod01_s.rim', mau)
            mod = mau['modules/mod01_s.rim']
            self.assertEqual(mod['module'], 'mod01')
            self.assertEqual(mod['actions']['6']['occurrences'], 1)   # a_main in this rim
            self.assertEqual(mod['actions']['574']['occurrences'], 1) # m_only
            self.assertEqual(mod['action_ops'], 2)
            self.assertNotIn('modules/mod01.rim', mau)  # no NCS in the area RIM

            self.assertEqual(report['decompile_status']['files'], 1)
            self.assertEqual(report['decompile_status']['classes']['exact']['count'], 1)
            bp = report['bug_parity_arity_anomalies']
            self.assertEqual(len(bp), 1)
            self.assertEqual(bp[0]['resref'], 'm_only')
            self.assertEqual(bp[0]['action_name'], 'Act574')


if __name__ == '__main__':
    unittest.main()
