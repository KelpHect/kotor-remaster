#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/scripts/actions.py (action table + usage).

No game data: the 772-entry table is synthesized in a temp file and the NCS
payloads are built with tools/scripts/ncs.py. Run from the repo root:

    python -m unittest discover -s tools/scripts/tests -v
"""
from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from scripts import ncs, actions  # noqa: E402


def make_table(path, arg_specs=None):
    """Write a synthetic 772-entry action table; arg_specs maps id -> args string."""
    arg_specs = arg_specs or {}
    acts = []
    for i in range(actions.ACTION_COUNT):
        acts.append({'id': i, 'name': 'Act%03d' % i,
                     'args': arg_specs.get(i, ''), 'handler': 'H_%x' % i})
    doc = {'build_sha256': None, 'actions': acts,
           'dispatch': {'entry': '0x0052c0d0'}}
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(doc, f)
    return path


def ncs_with_actions(*pairs):
    """Build an NCS payload whose stream is ACTION ops followed by RETN."""
    insns = []
    for aid, argc in pairs:
        insns.append(ncs.Instruction(offset=0, opcode=0x05, name='ACTION', shape='action',
                                     qualifier=0x20,
                                     ops=[('c', struct.pack('>HB', aid, argc))],
                                     raw=bytes([0x20]) + struct.pack('>HB', aid, argc)))
    insns.append(ncs.Instruction(offset=0, opcode=0x20, name='RETN', shape='qual',
                                 qualifier=0x03, ops=[], raw=b'\x03'))
    return ncs.encode(insns, 'readback')


class TestActionTable(unittest.TestCase):
    def test_split_params(self):
        # note: split is on top-level commas only; string literals with commas
        # are not expected in nwscript prototypes (same rule as lane 04)
        self.assertEqual(actions.split_params('int a, object o, string s = "xy"'),
                         ['int a', 'object o', 'string s = "xy"'])
        # nested parens protect the comma
        self.assertEqual(actions.split_params('effect e = EffectLinkEffects(a, b), int x'),
                         ['effect e = EffectLinkEffects(a, b)', 'int x'])
        self.assertEqual(actions.split_params(''), [])
        self.assertEqual(actions.split_params('void'), ['void'])

    def test_load_and_arity(self):
        with tempfile.TemporaryDirectory() as d:
            path = make_table(os.path.join(d, 't.json'), {
                0: 'int a, int b',              # total 2 required 2
                1: 'int a, int b = 1',         # total 2 required 1
                2: '',                          # total 0 required 0
            })
            t = actions.load_action_table(path)
        self.assertEqual(len(t['actions']), 772)
        self.assertEqual(t['arity'][0], {'name': 'Act000', 'args': 'int a, int b',
                                         'handler': 'H_0', 'total': 2, 'required': 2})
        self.assertEqual(t['arity'][1]['required'], 1)
        self.assertEqual(t['arity'][2]['total'], 0)

    def test_load_rejects_wrong_size(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 't.json')
            with open(path, 'w') as f:
                json.dump({'actions': [{'id': 0, 'name': 'x', 'args': '', 'handler': 'h'}]}, f)
            with self.assertRaises(ValueError):
                actions.load_action_table(path)

    def test_arity_violation(self):
        with tempfile.TemporaryDirectory() as d:
            t = actions.load_action_table(make_table(os.path.join(d, 't.json'), {
                0: 'int a, int b', 1: 'int a, int b = 1'}))
        self.assertIsNone(actions.arity_violation(t, 0, 2))
        self.assertEqual(actions.arity_violation(t, 0, 1), 'action_missing_args')
        self.assertEqual(actions.arity_violation(t, 0, 3), 'action_extra_args')
        self.assertEqual(actions.arity_violation(t, 1, 1), None)
        self.assertEqual(actions.arity_violation(t, 1, 2), None)
        self.assertEqual(actions.arity_violation(t, 1, 3), 'action_extra_args')
        self.assertEqual(actions.arity_violation(t, 900, 1), 'unknown_id')


class TestUsage(unittest.TestCase):
    def test_scan_usage_counts_and_anomalies(self):
        with tempfile.TemporaryDirectory() as d:
            t = actions.load_action_table(make_table(os.path.join(d, 't.json'), {
                6: 'object o, int i',   # total 2 required 2
                574: 'int n, object o', # total 2 required 2
            }))
            data = ncs_with_actions((6, 2), (6, 2), (574, 1), (900, 0))
            import hashlib
            sha = hashlib.sha256(data).hexdigest()
            row = actions.scan_usage(t, sha, data, 'synth')
        self.assertEqual(row['action_ops'], 4)
        self.assertEqual(row['actions'], {6: 2, 574: 1, 900: 1})
        self.assertEqual(row['unknown_action_ops'], 1)
        kinds = sorted(a['kind'] for a in row['arity_anomalies'])
        self.assertEqual(kinds, ['action_missing_args', 'unknown_id'])
        miss = [a for a in row['arity_anomalies'] if a['kind'] == 'action_missing_args'][0]
        self.assertEqual(miss['action'], 574)
        self.assertEqual(miss['argc'], 1)
        self.assertEqual(miss['declared'], 2)

    def test_aggregate_module_usage(self):
        rows = [
            {'sha256': 'a', 'actions': {1: 2, 2: 1}, 'action_ops': 3},
            {'sha256': 'b', 'actions': {2: 5}, 'action_ops': 5},
            {'sha256': 'a', 'actions': {1: 2, 2: 1}, 'action_ops': 3},  # dup sha ignored by set
        ]
        agg = actions.aggregate_module_usage(rows)
        self.assertEqual(agg['actions'], {1: 4, 2: 7})  # rows summed as given
        self.assertEqual(agg['action_ops'], 11)
        self.assertEqual(agg['distinct_actions'], 2)
        self.assertEqual(agg['unique_files'], 2)


if __name__ == '__main__':
    unittest.main()
