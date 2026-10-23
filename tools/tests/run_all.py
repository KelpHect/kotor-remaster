#!/usr/bin/env python3
"""Run the lane 05 test suite and write a deterministic results JSON."""
from __future__ import annotations

import json
import sys
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent
RESULTS = REPO_ROOT / "artifacts" / "tests" / "results"
sys.path.insert(0, str(TESTS))

import support  # noqa: E402


class RecordingResult(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.rows: dict[str, dict] = {}

    def addSuccess(self, test):
        super().addSuccess(test)
        self.rows[test.id()] = {"test": test.id(), "status": "pass"}

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self.rows[test.id()] = {"test": test.id(), "status": "fail"}

    def addError(self, test, err):
        super().addError(test, err)
        self.rows[test.id()] = {"test": test.id(), "status": "error"}

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self.rows[test.id()] = {"test": test.id(), "status": "skip", "reason": reason}


def main() -> int:
    support.ensure_fixtures()
    suite = unittest.defaultTestLoader.discover(str(TESTS), pattern="test_*.py",
                                                top_level_dir=str(TESTS))
    result = unittest.TextTestRunner(verbosity=2, resultclass=RecordingResult).run(suite)
    rows = [result.rows[k] for k in sorted(result.rows)]
    passed = result.testsRun - len(result.failures) - len(result.errors) - len(result.skipped)
    rows.append({"summary": {
        "tests_run": result.testsRun,
        "passed": passed,
        "failed": len(result.failures),
        "errors": len(result.errors),
        "skipped": len(result.skipped),
        "real_install": str(support.REAL_GAME),
        "real_install_present": (support.REAL_GAME / "chitin.key").exists(),
        "real_saves": [str(p) for p in support.real_save_folders()],
        "generated_unix": int(time.time()),
    }})
    RESULTS.mkdir(parents=True, exist_ok=True)
    out = RESULTS / "test-results.json"
    out.write_text(json.dumps(rows, indent=2), encoding="utf-8")
    print(f"\nwrote {out}")
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
