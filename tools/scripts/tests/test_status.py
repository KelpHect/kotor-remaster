#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/scripts/status.py (decompile classes).

No game data: status rows are synthesized. Run from the repo root:

    python -m unittest discover -s tools/scripts/tests -v
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from scripts import status  # noqa: E402


def write_status(path, rows):
    with open(path, 'w', encoding='utf-8') as f:
        for r in rows:
            f.write(json.dumps(r) + '\n')
    return path


class TestStatus(unittest.TestCase):
    def test_classify_row_mapping(self):
        self.assertEqual(status.classify_row({'comparison_status': 'exact', 'status': 'exact'}),
                         status.CLASS_EXACT)
        self.assertEqual(status.classify_row({'comparison_status': 'mismatch', 'status': 'mismatch'}),
                         status.CLASS_COMPILER_LIMITED)
        self.assertEqual(status.classify_row({'comparison_status': 'reachable', 'status': 'reachable'}),
                         status.CLASS_REACHABLE_ONLY)
        self.assertEqual(status.classify_row({'comparison_status': None, 'status': 'compile_fail'}),
                         status.CLASS_HEADER_LIMITED)
        with self.assertRaises(ValueError):
            status.classify_row({'comparison_status': 'weird', 'status': 'weird'})

    def test_load_and_summarize(self):
        rows = (
            [{'input_sha256': '%064x' % i, 'comparison_status': 'exact', 'status': 'exact',
              'byte_equal': True, 'full_stream_equal': True, 'name': 's%d' % i}
             for i in range(97)]
            + [{'input_sha256': 'a' * 64, 'comparison_status': 'mismatch', 'status': 'mismatch',
                'name': 'mm'}]
            + [{'input_sha256': 'b' * 64, 'comparison_status': None, 'status': 'compile_fail',
                'compiled': False, 'name': 'cf',
                'notes': [['main', 'action_missing_args', 574, 1, 2]]}]
            + [{'input_sha256': 'c' * 64, 'comparison_status': 'reachable', 'status': 'reachable',
                'name': 'ro'}]
        )
        with tempfile.TemporaryDirectory() as d:
            st = status.load_status_jsonl(write_status(os.path.join(d, 's.jsonl'), rows))
            s = status.summarize(st)
        self.assertEqual(s['files'], 100)
        self.assertEqual(s['classes'][status.CLASS_EXACT]['count'], 97)
        self.assertEqual(s['classes'][status.CLASS_EXACT]['share_pct'], 97.0)
        self.assertEqual(s['classes'][status.CLASS_COMPILER_LIMITED]['count'], 1)
        self.assertEqual(s['classes'][status.CLASS_HEADER_LIMITED]['count'], 1)
        self.assertEqual(s['classes'][status.CLASS_REACHABLE_ONLY]['count'], 1)
        self.assertEqual(s['header_limited_files'], ['cf'])

    def test_bug_parity_items(self):
        rows = [{'resref': 'k_act_carthjoin', 'sha256': 'd' * 64,
                 'arity_anomalies': [
                     {'action': 574, 'name': 'AddPartyMember', 'argc': 1,
                      'declared': 2, 'required': 2, 'kind': 'action_missing_args'},
                     {'action': 6, 'name': 'AssignCommand', 'argc': 3,
                      'declared': 2, 'required': 2, 'kind': 'action_extra_args'}]}]
        items = status.bug_parity_items(rows)
        self.assertEqual(len(items), 2)
        self.assertEqual(items[0]['action_id'], 6)   # sorted by (resref, action_id)
        self.assertEqual(items[1]['kind'], 'action_missing_args')
        self.assertEqual(items[1]['action_name'], 'AddPartyMember')


if __name__ == '__main__':
    unittest.main()
