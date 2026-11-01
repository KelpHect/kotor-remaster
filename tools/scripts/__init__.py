"""Public script-corpus toolset for the KotOR 1:1 interoperability program.

Lane s1-04-scripts. Everything here is clean-room tooling: the NCS codec uses
only the documented big-endian NCS V1.0 bytecode layout (verified lossless on
the shipped corpus), and the action table is consumed as data produced by
``tools/analysis/extract_actions.py`` from the pinned build
SHA-256 ``0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6``.

No game assets, extracted payloads, or decompiled program text live in this
package. All bulk outputs go under ``artifacts/lanes/s1-04-scripts/``.

Modules:
    ncs      -- NCS V1.0 decode / re-encode codec (readback + semantic modes)
    actions  -- action-table loading, arity, per-file / per-module usage
    status   -- decompile-status classes and action-arity anomaly detection
    corpus   -- read-only full-install scan, NCS<->NSS pairing ledger
"""

__all__ = ["ncs", "actions", "status", "corpus"]

BUILD_SHA256 = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"
NCS_TYPE = 2010
NSS_TYPE = 2009
