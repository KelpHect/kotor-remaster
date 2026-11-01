#!/usr/bin/env python3
"""WOK (area walkmesh, resource type 2016) validator on top of bwm.py.

A WOK is a BWM V1.0 container with type field 1; it additionally carries
per-face normals.  Verified semantics (docs/recon/04, docs/recon/12):

* vertices are world-space already (do NOT add the LYT room origin or
  ``position``);
* ``position`` (header float triple at 0x3C) equals the room MDL's type-545
  walkmesh node local translation, bit-exact 453/453 sampled rooms;
* shipped area walkmeshes are right-handed +Z-up: every face satisfies
  ``cross(v1-v0, v2-v0) . normal > 0`` (checked as a warning here, since a
  handful of shipped files contain mirrored surfaces -- docs/recon/15).

Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass

from bwm import Bwm, parse_bwm  # noqa: F401  (re-exported)


class WokError(ValueError):
    pass


@dataclass
class WokStats:
    winding_positive: int
    winding_negative: int
    winding_zero: int
    min_normal_dot: float


def parse_wok(data: bytes, name: str = "<memory>") -> Bwm:
    """Parse a WOK payload: BWM with type 1 and face normals present."""
    b = parse_bwm(data, name, expect_type=1)
    if b.face_count and not b.normals:
        raise WokError(f"{name}: type-1 BWM has faces but no normal section")
    return b


def winding_stats(wok: Bwm) -> WokStats:
    """Compare face winding with stored normals (RH +Z-up => dot > 0)."""
    pos = neg = zero = 0
    min_dot = 1e30
    for f, n in zip(wok.faces, wok.normals):
        a, c, d = (wok.verts[i] for i in f)
        u = tuple(c[i] - a[i] for i in range(3))
        v = tuple(d[i] - a[i] for i in range(3))
        cr = (u[1] * v[2] - u[2] * v[1],
              u[2] * v[0] - u[0] * v[2],
              u[0] * v[1] - u[1] * v[0])
        dot = sum(cr[i] * n[i] for i in range(3))
        min_dot = min(min_dot, dot)
        if dot > 1e-6:
            pos += 1
        elif dot < -1e-6:
            neg += 1
        else:
            zero += 1
    return WokStats(pos, neg, zero, min_dot)


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one WOK resource (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    w = parse_wok(data, args.path)
    st = winding_stats(w)
    print(f"{args.path}: verts={w.vert_count} faces={w.face_count} "
          f"pos={tuple(round(v, 5) for v in w.position)} winding=+{st.winding_positive}"
          f"/-{st.winding_negative}/0{st.winding_zero} min_dot={st.min_normal_dot:.6g}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
