#!/usr/bin/env python3
"""Compare a retail state trace with an engine state trace (format: SC/trace/TRACE_SPEC.md).

    python compare.py retail.tsv engine.tsv [--only obj,cam,...] [--json report.json] [--anchors settle,panel,dlg,round] [--verbose]

Per record class prints PASS / FAIL / SKIP and, on FAIL, the first divergence: both frames, the key, the field, both values.
Frames are never compared (the two programs run at different speeds); state is compared at the `shot` anchors both traces
share, and event classes are compared as ordered sequences. Exit code 0 only if no class FAILs. SKIP means "not comparable
yet" (unverified convention, column missing on one side) and is printed with the reason, never counted as a pass.
"""
import argparse
import json
import math
import sys
from collections import defaultdict

# ------------------------------------------------------------------ tolerances (TRACE_SPEC.md section 6)
TOL = {
    "pos": 0.05,          # metres, per axis (obj position, camera eye, dialogue eye, 3D sound position 0.10 below)
    "snd_pos": 0.10,
    "face_deg": 3.0,      # angle between orientation vectors
    "fov_deg": 0.5,
    "cam_deg": 1.0,       # angle between camera forward (and up) vectors
    "clip_time": 0.15,    # seconds; informational unless --strict-clip-time
    "hp": 0,              # exact
    "hover_pt": None,     # not compared
}

CLASSES = ["mod", "area", "pause", "party", "obj", "cam", "camm", "sel", "gui_stack", "gui", "ctl", "dlg", "dlgcam", "snd", "act", "rnd", "shot"]
CAMM_MAP = {"restore": "chase", "follow": "chase"}   # retail setter names -> engine mode names (extend in phase 2)


class Rec:
    __slots__ = ("frame", "t", "gt", "kind", "f", "line")

    def __init__(self, line):
        c = line.rstrip("\n").split("\t")
        self.line = line
        self.frame = int(c[0])
        self.t = int(c[1])
        self.gt = c[2]
        self.kind = c[3]
        self.f = c[4:]


def load(path):
    recs, meta = [], {}
    for ln in open(path, encoding="utf-8"):
        if not ln.strip():
            continue
        if ln.startswith("#"):
            c = ln.rstrip("\n").split("\t")
            if c[0] == "#meta" and len(c) >= 3:
                meta[c[1]] = c[2]
            continue
        try:
            recs.append(Rec(ln))
        except (ValueError, IndexError):
            print("bad line:", ln[:120], file=sys.stderr)
    return recs, meta


def fl(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def vdist(a, b):
    return max(abs(x - y) for x, y in zip(a, b))


def ang_between(a, b):
    na, nb = math.hypot(*a), math.hypot(*b)
    if na < 1e-6 or nb < 1e-6:
        return 0.0
    d = max(-1.0, min(1.0, (a[0] * b[0] + a[1] * b[1]) / (na * nb)))
    return math.degrees(math.acos(d))


class Diff:
    def __init__(self, cls):
        self.cls, self.first, self.n, self.skip, self.checked = cls, None, 0, None, 0

    def bad(self, rf, ef, key, field, rv, ev):
        self.n += 1
        if self.first is None:
            self.first = {"retail_frame": rf, "engine_frame": ef, "key": key, "field": field, "retail": rv, "engine": ev}

    def status(self):
        if self.skip:
            return "SKIP"
        return "FAIL" if self.n else "PASS"


# ------------------------------------------------------------------ snapshots at shared anchors
def snapshots(recs, anchors):
    st = {"obj": {}, "cam": None, "party": None, "sel": None, "gui_stack": None, "area": None, "mod": None, "camm": None, "pause": None}
    snaps, seen = {}, defaultdict(int)
    for r in recs:
        k, f = r.kind, r.f
        if k == "obj" and len(f) >= 14:
            st["obj"][f[0]] = (r.frame, f)
        elif k == "obj_del":
            st["obj"].pop(f[0], None)
        elif k in ("cam", "party", "sel", "gui_stack", "area", "pause"):
            st[k] = (r.frame, f)
        elif k == "mod" and f[0] == "ready":
            st["mod"] = (r.frame, f)
            st["obj"] = {}
        elif k == "camm":
            st["camm"] = (r.frame, f)
        elif k == "shot":
            name = f[0]
            if not any(name.startswith(a) for a in anchors):
                continue
            seen[name] += 1
            snaps[(name, seen[name])] = {kk: (dict(v) if kk == "obj" else v) for kk, v in st.items()}
    return snaps


def tagof(key):
    return key.rsplit("#", 1)[0].lower()


def cmp_obj(sr, se, d):
    """Pair objects by (tag, template) then nearest position; compare the fields; report objects present on one side only."""
    g_r, g_e = defaultdict(list), defaultdict(list)
    for k, (fr, f) in sr["obj"].items():
        g_r[(tagof(f[0]), f[3].lower())].append((fr, f))
    for k, (fr, f) in se["obj"].items():
        g_e[(tagof(f[0]), f[3].lower())].append((fr, f))
    for grp in sorted(set(g_r) | set(g_e)):
        lr, le = list(g_r.get(grp, [])), list(g_e.get(grp, []))
        pairs = []
        while lr and le:
            best = None
            for i, (frr, a) in enumerate(lr):
                pa = [fl(a[4]), fl(a[5]), fl(a[6])]
                for j, (fre, b) in enumerate(le):
                    pb = [fl(b[4]), fl(b[5]), fl(b[6])]
                    dd = vdist(pa, pb)
                    if best is None or dd < best[0]:
                        best = (dd, i, j)
            _, i, j = best
            pairs.append((lr.pop(i), le.pop(j)))
        for frr, a in lr:
            d.bad(frr, None, a[0], "present", "retail only", "-")
        for fre, b in le:
            d.bad(None, fre, b[0], "present", "-", "engine only")
        for (frr, a), (fre, b) in pairs:
            d.checked += 1
            key = a[0] + " ~ " + b[0]
            pa, pb = [fl(a[4]), fl(a[5]), fl(a[6])], [fl(b[4]), fl(b[5]), fl(b[6])]
            if vdist(pa, pb) > TOL["pos"]:
                d.bad(frr, fre, key, "pos", pa, pb)
            fa, fb = (fl(a[7]), fl(a[8])), (fl(b[7]), fl(b[8]))
            if None not in fa and None not in fb and ang_between(fa, fb) > TOL["face_deg"]:
                d.bad(frr, fre, key, "facing", fa, fb)
            if a[9] != "-" and b[9] != "-" and abs(int(a[9]) - int(b[9])) > TOL["hp"]:
                d.bad(frr, fre, key, "hp", a[9], b[9])
            for idx, name in ((10, "visible"), (11, "appearance")):
                if a[idx] != "-" and b[idx] != "-" and a[idx] != b[idx]:
                    d.bad(frr, fre, key, name, a[idx], b[idx])
            ca, cb = a[13].split("|")[0].split("@")[0].lower(), b[13].split("|")[0].split("@")[0].lower()
            if ca != "-" and cb != "-" and ca != cb:
                d.bad(frr, fre, key, "clip", a[13], b[13])


def cmp_cam(sr, se, d, meta_r, meta_e):
    a, b = sr["cam"], se["cam"]
    if a is None and b is None:
        return                      # menus and loading screens: no camera on either side
    if a is None or b is None:
        d.bad(a and a[0], b and b[0], "cam", "present", "yes" if a else "no", "yes" if b else "no")
        return
    (fr, x), (fe, y) = a, b
    d.checked += 1
    if vdist([fl(v) for v in x[0:3]], [fl(v) for v in y[0:3]]) > TOL["pos"]:
        d.bad(fr, fe, "cam", "eye", x[0:3], y[0:3])
    if abs(fl(x[9]) - fl(y[9])) > TOL["fov_deg"]:
        d.bad(fr, fe, "cam", "fov", x[9], y[9])
    for nm, lo in (("forward", 3), ("up", 6)):
        v1, v2 = [fl(v) for v in x[lo:lo + 3]], [fl(v) for v in y[lo:lo + 3]]
        n1, n2 = math.sqrt(sum(t * t for t in v1)) or 1.0, math.sqrt(sum(t * t for t in v2)) or 1.0
        ang = math.degrees(math.acos(max(-1.0, min(1.0, sum(p * q for p, q in zip(v1, v2)) / (n1 * n2)))))
        if ang > TOL["cam_deg"]:
            d.bad(fr, fe, "cam", nm + "(angle %.2f deg)" % ang, x[lo:lo + 3], y[lo:lo + 3])


def seq_cmp(d, ra, rb, keyf, label):
    """Ordered sequences of event keys; first divergence."""
    ka, kb = [(r.frame, keyf(r)) for r in ra], [(r.frame, keyf(r)) for r in rb]
    d.checked = max(len(ka), len(kb))
    for i in range(max(len(ka), len(kb))):
        x = ka[i] if i < len(ka) else (None, None)
        y = kb[i] if i < len(kb) else (None, None)
        if x[1] != y[1]:
            d.bad(x[0], y[0], f"{label}[{i}]", "event", x[1], y[1])
            return


def run(args):
    rr, meta_r = load(args.retail)
    ee, meta_e = load(args.engine)
    anchors = tuple(args.anchors.split(","))
    only = set(args.only.split(",")) if args.only else set(CLASSES)
    out = {}

    def sel(recs, kind):
        return [r for r in recs if r.kind == kind]

    # event classes ---------------------------------------------------------------
    d = Diff("mod")
    seq_cmp(d, [r for r in sel(rr, "mod") if r.f[0] == "ready"], [r for r in sel(ee, "mod") if r.f[0] == "ready"], lambda r: r.f[1].lower(), "mod")
    out["mod"] = d
    d = Diff("area")
    dedupe = lambda recs: [r for i, r in enumerate(recs) if i == 0 or r.f != recs[i - 1].f]
    d.skip = "retail keeps no area tag or resref in the area object (phase-2 probe: not at +0x18/+0x158, no string pointer in the first 0x600 bytes); area identity is the module name (mod)"
    out["area"] = d
    d = Diff("camm")
    cm = lambda r: CAMM_MAP.get(r.f[0], r.f[0])
    seq_cmp(d, dedupe(sel(rr, "camm")), dedupe(sel(ee, "camm")), cm, "camm")
    out["camm"] = d
    d = Diff("gui")
    seq_cmp(d, [r for r in sel(rr, "gui") if r.f[0] == "open"], [r for r in sel(ee, "gui") if r.f[0] == "open"], lambda r: (r.f[1].lower(), tuple(r.f[3:7])), "gui open")
    out["gui"] = d
    d = Diff("gui_stack")
    seq_cmp(d, dedupe(sel(rr, "gui_stack")), dedupe(sel(ee, "gui_stack")), lambda r: r.f[0].lower(), "gui_stack")
    out["gui_stack"] = d
    d = Diff("dlg")
    # dlg line <resref> <n> owner speaker listener angle camId nAnims textHash [raw]: tags compared without the #n, the resref and n are not (retail may report none)
    dk = lambda r: (tagof(r.f[3]), tagof(r.f[4]), tagof(r.f[5]), r.f[6], r.f[7], r.f[8], r.f[9] if r.f[9] != "-" else "*")
    if any(len(r.f) >= 10 for r in sel(ee, "dlg")):
        ra, ea = sel(rr, "dlg"), sel(ee, "dlg")
        d.checked = max(len(ra), len(ea))
        for i in range(d.checked):
            x = ra[i] if i < len(ra) else None
            y = ea[i] if i < len(ea) else None
            kx, ky = (dk(x) if x else None), (dk(y) if y else None)
            if kx is None or ky is None or any(p != q and "*" not in (p, q) for p, q in zip(kx, ky)):
                d.bad(x.frame if x else None, y.frame if y else None, "dlg[%d]" % i, "line", kx, ky)
                break
    else:
        d.skip = "engine trace has no dlg records (TRACE_SPEC 4.9)"
    out["dlg"] = d
    d = Diff("dlgcam")
    seq_cmp(d, [r for r in sel(rr, "dlgcam") if r.f[0] == "eye"], [r for r in sel(ee, "dlgcam") if r.f[0] == "eye"], lambda r: (r.f[1], r.f[2]), "dlgcam")
    out["dlgcam"] = d
    d = Diff("snd")
    seq_cmp(d, [r for r in sel(rr, "snd") if r.f[0] == "start"], [r for r in sel(ee, "snd") if r.f[0] == "start"], lambda r: (r.f[1][:6], r.f[2].lower()), "snd")
    out["snd"] = d
    d = Diff("act")
    seq_cmp(d, sel(rr, "act"), sel(ee, "act"), lambda r: r.f[0], "act")
    out["act"] = d
    d = Diff("rnd")
    seq_cmp(d, sel(rr, "rnd"), sel(ee, "rnd"), lambda r: (r.f[0], tagof(r.f[2]) if len(r.f) > 2 else "-"), "rnd")   # n is a per-run counter, the owner key is what must match
    out["rnd"] = d
    d = Diff("pause")
    seq_cmp(d, dedupe(sel(rr, "pause")), dedupe(sel(ee, "pause")), lambda r: r.f[0], "pause")
    out["pause"] = d
    d = Diff("shot")
    seq_cmp(d, sel(rr, "shot"), sel(ee, "shot"), lambda r: r.f[0], "shot")
    out["shot"] = d
    d = Diff("ctl")
    # by (panel, id): the latest rect of each id wins (the retail HUD panel grows after it opens); id < 0 are runtime-created controls and are ignored
    cr, ce = defaultdict(dict), defaultdict(dict)
    for recs, dst in ((sel(rr, "ctl"), cr), (sel(ee, "ctl"), ce)):
        for r in recs:
            if int(r.f[1]) >= 0:
                dst[r.f[0].lower()][int(r.f[1])] = tuple(int(v) for v in r.f[2:6])
    for res in sorted(set(cr) | set(ce)):
        d.checked += 1
        if cr[res] != ce[res]:
            ids = sorted(set(cr[res]) ^ set(ce[res])) or sorted(i for i in cr[res] if cr[res][i] != ce[res].get(i))
            i0 = ids[0]
            d.bad(None, None, res, "control id %d" % i0, cr[res].get(i0), ce[res].get(i0))
    out["ctl"] = d

    # state classes at anchors ------------------------------------------------------
    sr, se = snapshots(rr, anchors), snapshots(ee, anchors)
    common = [k for k in sr if k in se]
    d_obj, d_cam, d_party, d_sel = Diff("obj"), Diff("cam"), Diff("party"), Diff("sel")
    d_cam.skip_orient = False
    if not common:
        for dd in (d_obj, d_cam, d_party, d_sel):
            dd.skip = "no shared anchors (" + ", ".join(anchors) + ") in both traces"
    for k in common:
        cmp_obj(sr[k], se[k], d_obj)
        cmp_cam(sr[k], se[k], d_cam, meta_r, meta_e)
        pr, pe_ = sr[k]["party"], se[k]["party"]
        if pr and pe_:
            d_party.checked += 1
            norm = lambda f: (tagof(f[0]), sorted(tagof(x) for x in f[1].split(",")))
            if norm(pr[1]) != norm(pe_[1]):
                d_party.bad(pr[0], pe_[0], k[0], "leader/members", pr[1], pe_[1])
        a, b = sr[k]["sel"], se[k]["sel"]
        if a and b:
            d_sel.checked += 1
            if tagof(a[1][0]) != tagof(b[1][0]):
                d_sel.bad(a[0], b[0], k[0], "target", a[1][0], b[1][0])
    out["obj"], out["cam"], out["party"], out["sel"] = d_obj, d_cam, d_party, d_sel

    # report ------------------------------------------------------------------------
    rows, fail = [], 0
    for c in CLASSES:
        if c not in only:
            continue
        d = out[c]
        if not d.skip and d.checked == 0 and d.n == 0:
            d.skip = "no records of this class in either trace"
        s = d.status()
        fail += s == "FAIL"
        line = f"{s:5} {c:10} checked={d.checked:<5}"
        if s == "FAIL":
            line += f" mismatches={d.n} first={json.dumps(d.first)}"
        elif s == "SKIP":
            line += " reason=" + d.skip
        elif getattr(d, "note", None):
            line += " note=" + d.note
        rows.append(line)
    print(f"retail={args.retail} engine={args.engine} anchors_shared={len(common)} tolerances={json.dumps({k: v for k, v in TOL.items() if v is not None})}")
    print("\n".join(rows))
    if args.json:
        json.dump({c: {"status": out[c].status(), "checked": out[c].checked, "mismatches": out[c].n, "first": out[c].first, "skip": out[c].skip} for c in CLASSES if c in only},
                  open(args.json, "w"), indent=1)
    return 1 if fail else 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("retail")
    ap.add_argument("engine")
    ap.add_argument("--only")
    ap.add_argument("--json")
    ap.add_argument("--anchors", default="settle,panel,dlg,round")
    ap.add_argument("--verbose", action="store_true")
    sys.exit(run(ap.parse_args()))
