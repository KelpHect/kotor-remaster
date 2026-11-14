#!/usr/bin/env python3
"""Segment-aligned divergence report between a retail state trace and an engine state trace (TRACE_SPEC.md).

    python tdiff.py retail.tsv engine.tsv --sample NAME [--out DIR] [--top 25]

Writes <out>/DIFF.tsv (default out = SC/trace/diff/<NAME>/) and prints a one-screen summary.

How the traces are aligned (compare.py matched anchors by name and occurrence from the file start, which compared
different moments):
  * Rows before the first `mod ready` (menu, fade, save/load screens, loading screen) are the "load" segment and are
    not compared; they are only listed as a count.
  * Each `mod ready <module>` starts a segment that runs to the next `mod ready`. Segments are paired by module name
    and occurrence; a module present on one side only is one divergence row.
  * Inside a segment, state is compared at the moment the HUD is up (`shot panel:mipc210x7`, 12 swaps after it opened,
    present on both sides) and at each `dlg:-:n` anchor; objects use their first-sight row (initial state), so the
    comparison does not depend on how long each program idled. Event classes are compared as multisets (what happened)
    and, for dlg / dlgcam / camm / pause / gui, in order per index.

A divergence "class" is a rule, not a frame: every row of DIFF.tsv carries a count of the objects, lines or events the
rule covers, the first frame and module time, an example from each side and the object tag.
Rows that cannot be read (old-format dlg rows, bad lines) are skipped with a note on stderr and in the summary.
"""
import argparse
import difflib
import math
import os
import re
import sys
from collections import Counter, defaultdict

TOL = {"pos": 0.05, "pair": 2.0, "face_deg": 3.0, "fov_deg": 0.5, "cam_deg": 1.0, "snd_pos": 0.10, "time_s": 3.0}
GT_MODULE = re.compile(r"^0:(\d+)$")
GT_ANY = re.compile(r"^(\d+):(\d+)$")
NOTES = []


class Rec:
    __slots__ = ("frame", "t", "gt", "kind", "f", "mt")

    def __init__(self, cols):
        self.frame = int(cols[0])
        self.t = int(cols[1])
        self.gt = cols[2]
        self.kind = cols[3]
        self.f = cols[4:]
        self.mt = None            # module clock in ms (filled by segment)


def load(path):
    recs, meta, bad = [], {}, 0
    for ln in open(path, encoding="utf-8", errors="replace"):
        if not ln.strip():
            continue
        if ln.startswith("#"):
            c = ln.rstrip("\n").split("\t")
            if c[0] == "#meta" and len(c) >= 3:
                meta[c[1]] = c[2]
            continue
        c = ln.rstrip("\n").split("\t")
        try:
            recs.append(Rec(c))
        except (ValueError, IndexError):
            bad += 1
    if bad:
        NOTES.append("%s: %d unreadable lines skipped" % (os.path.basename(path), bad))
    return recs, meta


def fl(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def tagof(key):
    return key.rsplit("#", 1)[0].lower()


def norm_panel(name):
    return "anon" if name.lower().startswith("anon@") else name.lower()


def angle(a, b):
    na, nb = math.hypot(*a), math.hypot(*b)
    if na < 1e-6 or nb < 1e-6:
        return 0.0
    return math.degrees(math.acos(max(-1.0, min(1.0, (a[0] * b[0] + a[1] * b[1]) / (na * nb)))))


# ----------------------------------------------------------------------------------------------- segments
class Seg:
    def __init__(self, module, index, recs, ready, carry):
        self.module, self.index, self.recs, self.ready = module, index, recs, ready
        gm = GT_MODULE.match(ready.gt)
        base = int(gm.group(1)) if gm else None
        self.carry = carry        # last state row of each class before this segment (pause, party, ... carry over a load)
        # One clock for both sides: the game clock in ms since the module clock started. The engine's clock reads 0:0 at
        # the ready row; a retail save keeps its own clock (any day, a time of day in the hundreds of thousands), and
        # the clock stands still while the game is paused or fading in. The ready row of a retail load shows the clock
        # of the previous state (day 450381), so when it is not `0:<ms>` the first later row with a clock is the base,
        # and a row without a readable clock keeps the last value (wall time would count paused seconds as game time).
        def clock(gt):
            m = GT_ANY.match(gt)
            return int(m.group(1)) * 86400000 + int(m.group(2)) if m else None
        if base is None:
            first = next((clock(r.gt) for r in recs if r.gt != ready.gt and clock(r.gt) is not None), None)
            base = first
        else:
            base = clock(ready.gt)
        last = 0
        for r in recs:
            c = clock(r.gt)
            if c is not None and base is not None and (r.gt != ready.gt or gm):
                last = max(last, c - base)
            r.mt = last if base is not None else max(0, r.t - ready.t)
        # the HUD anchor: the shot 12 swaps after mipc210x7 opened
        self.end = max((r.mt for r in recs), default=0)
        self.limit = None         # module time (ms) up to which event classes are compared (the shorter run)
        self.hud = next((i for i, r in enumerate(recs) if r.kind == "shot" and r.f[0] == "panel:mipc210x7"), None)
        self.settle = next((i for i, r in enumerate(recs) if r.kind == "shot" and r.f[0].lower().startswith("settle:")), None)

    def first(self, kind, pred=None):
        return next((r for r in self.recs if r.kind == kind and (pred is None or pred(r))), None)


STATE_KINDS = ("cam", "party", "sel", "camm", "pause", "gui_stack", "area")


def segments(recs):
    pre, segs, cur = [], [], None
    seen = Counter()
    carry = {}
    for r in recs:
        if r.kind == "mod" and r.f and r.f[0] == "ready" and len(r.f) > 1:
            name = r.f[1].lower()
            seen[name] += 1
            cur = [name, seen[name], r, [], dict(carry)]
            segs.append(cur)
        (cur[3] if cur else pre).append(r)
        if r.kind in STATE_KINDS:
            carry[r.kind] = r
    return pre, [Seg(m, i, rows, ready, cy) for m, i, ready, rows, cy in segs]


def state_at(recs, upto, carry=None):
    """Last row of each state class up to index `upto` (inclusive), starting from the state carried into the segment."""
    st = dict(carry or {})
    for r in recs[:upto + 1]:
        if r.kind in STATE_KINDS:
            st[r.kind] = r
    return st


# ----------------------------------------------------------------------------------------------- findings
class Findings:
    def __init__(self):
        self.rows = {}

    def add(self, cls, field, rule, rec_r=None, rec_e=None, ex_r="", ex_e="", tag="-", segment="-", n=1, kind="engine"):
        key = (cls, field, rule)
        row = self.rows.get(key)
        if row is None:
            row = self.rows[key] = {"class": cls, "field": field, "rule": rule, "count": 0, "frame_r": "-", "frame_e": "-",
                                    "time_ms": "-", "ex_r": ex_r, "ex_e": ex_e, "tag": tag, "segment": segment}
            if rec_r is not None:
                row["frame_r"] = rec_r.frame
                row["time_ms"] = rec_r.mt if rec_r.mt is not None else "-"
            if rec_e is not None:
                row["frame_e"] = rec_e.frame
                if row["time_ms"] == "-":
                    row["time_ms"] = rec_e.mt if rec_e.mt is not None else "-"
        row["count"] += n


def clip(s, n=90):
    s = str(s)
    return s if len(s) <= n else s[:n - 3] + "..."


# ----------------------------------------------------------------------------------------------- objects
def first_sight(seg):
    """key -> first obj row with a real position (retail writes a row at 0,0,0 before an object is placed)."""
    out, zero = {}, {}
    for r in seg.recs:
        if r.kind == "obj" and len(r.f) >= 14:
            k = r.f[0] + "@" + r.f[1]  # retail can reuse a tag#N label for two objects; the object id keeps them apart
            if k in out:
                continue
            if all(abs(fl(v) or 0.0) < 1e-6 for v in r.f[4:7]):
                zero.setdefault(k, r)
            else:
                out[k] = r
    for k, r in zero.items():
        out.setdefault(k, r)
    return out


def pair_objects(R, E):
    """Pair by (type, tag): nearest initial position within TOL['pair'], then by ordinal. Returns pairs, r_only, e_only."""
    gr, ge = defaultdict(list), defaultdict(list)
    for k, r in R.items():
        gr[(r.f[2], tagof(k))].append(r)
    for k, r in E.items():
        ge[(r.f[2], tagof(k))].append(r)
    pairs, r_only, e_only = [], [], []
    for grp in sorted(set(gr) | set(ge)):
        lr, le = list(gr.get(grp, [])), list(ge.get(grp, []))
        cand = []
        for i, a in enumerate(lr):
            pa = (fl(a.f[4]), fl(a.f[5]), fl(a.f[6]))
            for j, b in enumerate(le):
                pb = (fl(b.f[4]), fl(b.f[5]), fl(b.f[6]))
                if None in pa or None in pb:
                    continue
                d = math.dist(pa, pb)
                if d <= TOL["pair"]:
                    cand.append((d, i, j))
        cand.sort()
        used_r, used_e = set(), set()
        for d, i, j in cand:
            if i in used_r or j in used_e:
                continue
            used_r.add(i)
            used_e.add(j)
            pairs.append((lr[i], le[j], False))
        rest_r = [a for i, a in enumerate(lr) if i not in used_r]
        rest_e = [b for j, b in enumerate(le) if j not in used_e]
        rest_r.sort(key=lambda a: int(a.f[0].rsplit("#", 1)[1]) if "#" in a.f[0] else 0)
        rest_e.sort(key=lambda a: int(a.f[0].rsplit("#", 1)[1]) if "#" in a.f[0] else 0)
        for a, b in zip(rest_r, rest_e):
            pairs.append((a, b, True))
        r_only += rest_r[len(rest_e):]
        e_only += rest_e[len(rest_r):]
    return pairs, r_only, e_only


def cmp_objects(sr, se, F, seg):
    R, E = first_sight(sr), first_sight(se)
    pairs, r_only, e_only = pair_objects(R, E)
    by_type = defaultdict(list)
    for a in r_only:
        by_type[("retail only", a.f[2])].append(a)
    for b in e_only:
        by_type[("engine only", b.f[2])].append(b)
    for (side, typ), rows in by_type.items():
        tags = Counter(tagof(x.f[0]) for x in rows)
        top = ", ".join("%s x%d" % t for t in tags.most_common(6))
        rr = rows[0] if side == "retail only" else None
        ee = rows[0] if side == "engine only" else None
        F.add("obj", "unpaired", "%s %s objects with no counterpart (tag + position within %.1f m)" % (side, typ, TOL["pair"]),
              rr, ee, top if side == "retail only" else "-", top if side == "engine only" else "-", tags.most_common(1)[0][0],
              seg, n=len(rows))
    # field differences
    pat = defaultdict(Counter)
    for a, b, ordinal in pairs:
        typ = a.f[2]
        tag = tagof(a.f[0])
        pa, pb = [fl(a.f[4]), fl(a.f[5]), fl(a.f[6])], [fl(b.f[4]), fl(b.f[5]), fl(b.f[6])]
        if None not in pa and None not in pb:
            dist = max(abs(p - q) for p, q in zip(pa, pb))
            if dist > TOL["pos"]:
                F.add("obj", "pos", "%s initial position differs by more than %.2f m (first-sight row)" % (typ, TOL["pos"]), a, b,
                      "%s %s" % (a.f[0], a.f[4:7]), "%s %s" % (b.f[0], b.f[4:7]), tag, seg)
        fa, fb = (fl(a.f[7]), fl(a.f[8])), (fl(b.f[7]), fl(b.f[8]))
        if None not in fa and None not in fb and angle(fa, fb) > TOL["face_deg"]:
            F.add("obj", "facing", "%s initial facing differs by more than %.0f deg" % (typ, TOL["face_deg"]), a, b,
                  "%s %s" % (a.f[0], a.f[7:9]), "%s %s" % (b.f[0], b.f[7:9]), tag, seg)
        for idx, name in ((9, "hp"), (10, "visible"), (11, "appearance")):
            va, vb = a.f[idx], b.f[idx]
            if va != "-" and vb != "-" and va != vb:
                pat[(typ, name)][(va, vb)] += 1
                F.add("obj", name, "%s %s differs" % (typ, name), a, b, "%s %s=%s" % (a.f[0], name, va),
                      "%s %s=%s" % (b.f[0], name, vb), tag, seg)
        ca, cb = a.f[13].split("|")[0].split("@")[0].lower(), b.f[13].split("|")[0].split("@")[0].lower()
        if ca != "-" and cb != "-" and ca != cb:
            F.add("obj", "clip", "%s first clip name differs" % typ, a, b, "%s %s" % (a.f[0], a.f[13]), "%s %s" % (b.f[0], b.f[13]),
                  tag, seg)
        ta, tb = a.f[3].lower(), b.f[3].lower()
        if tb != "-" and ta != tb:
            F.add("obj", "template", "retail template unusable (`-` or a stub like `po_`), not comparable; engine has the resref", a, b,
                  "%s template=%s" % (a.f[0], a.f[3]), "%s template=%s" % (b.f[0], b.f[3]), tag, seg)
        if ordinal:
            F.add("obj", "pairing", "%s paired by ordinal only (no counterpart within %.1f m)" % (typ, TOL["pair"]), a, b,
                  "%s %s" % (a.f[0], a.f[4:7]), "%s %s" % (b.f[0], b.f[4:7]), tag, seg)
    # fold the most common value pair into the rule text
    for (typ, name), c in pat.items():
        (va, vb), n = c.most_common(1)[0]
        key = (("obj", name, "%s %s differs" % (typ, name)))
        if key in F.rows:
            F.rows[key]["rule"] = "%s %s differs (most common: retail %s, engine %s, %d of %d)" % (
                typ, name, va, vb, n, F.rows[key]["count"])
    return len(pairs)


# ----------------------------------------------------------------------------------------------- state at the HUD anchor
def vec(row, lo, n=3):
    return [fl(v) for v in row.f[lo:lo + n]]


def cmp_state(sr, se, F, seg):
    # the HUD anchor when both runs have one (a headless run has none): else the settle anchor, else nothing to compare
    if sr.hud is not None and se.hud is not None:
        ia, ib = sr.hud, se.hud
    elif sr.settle is not None and se.settle is not None:
        ia, ib = sr.settle, se.settle
    else:
        if (sr.hud is None) != (se.hud is None):
            F.add("shot", "anchor", "panel:mipc210x7 anchor missing on %s" % ("retail" if sr.hud is None else "engine"),
                  sr.ready, se.ready, "-", "-", "-", seg, kind="state")
        return
    a, b = state_at(sr.recs, ia, sr.carry), state_at(se.recs, ib, se.carry)
    ca, cb = a.get("cam"), b.get("cam")
    if ca and cb:
        x, y = ca, cb
        d = max(abs(p - q) for p, q in zip(vec(x, 0), vec(y, 0)))
        if d > TOL["pos"]:
            F.add("cam", "eye", "camera eye differs at the HUD anchor by more than %.2f m" % TOL["pos"], x, y, x.f[0:3], y.f[0:3], "-", seg)
        if abs(fl(x.f[9]) - fl(y.f[9])) > TOL["fov_deg"]:
            F.add("cam", "fov", "camera fov differs", x, y, x.f[9], y.f[9], "-", seg)
        for nm, lo in (("forward", 3), ("up", 6)):
            v1, v2 = vec(x, lo), vec(y, lo)
            n1, n2 = math.sqrt(sum(t * t for t in v1)) or 1.0, math.sqrt(sum(t * t for t in v2)) or 1.0
            ang = math.degrees(math.acos(max(-1.0, min(1.0, sum(p * q for p, q in zip(v1, v2)) / (n1 * n2)))))
            if ang > TOL["cam_deg"]:
                F.add("cam", nm, "camera %s differs by more than %.0f deg at the HUD anchor" % (nm, TOL["cam_deg"]), x, y,
                      "%s (%.1f deg)" % (x.f[lo:lo + 3], ang), y.f[lo:lo + 3], "-", seg)
        if abs(fl(x.f[10]) - fl(y.f[10])) > 1e-3 or abs(fl(x.f[11]) - fl(y.f[11])) > 1.0:
            F.add("cam", "clip", "camera near/far plane differs", x, y, x.f[10:12], y.f[10:12], "-", seg)
    elif ca or cb:
        F.add("cam", "present", "camera row missing on %s at the HUD anchor" % ("engine" if ca else "retail"), ca, cb, "-", "-", "-", seg)
    for kind, name in (("party", "leader/members"), ("pause", "pause state"), ("camm", "camera mode")):
        x, y = a.get(kind), b.get(kind)
        if kind == "camm":
            va = [{"restore": "chase", "follow": "chase"}.get(v.lower(), v.lower()) for v in (x.f[:1] if x else ["-"])]
            vb = [{"restore": "chase", "follow": "chase"}.get(v.lower(), v.lower()) for v in (y.f[:1] if y else ["-"])]
        elif kind == "party":
            va = [v.lower() for v in (x.f[:2] if x else ["-", "-"])]
            vb = [v.lower() for v in (y.f[:2] if y else ["-", "-"])]
        else:
            va, vb = (x.f[:1] if x else ["-"]), (y.f[:1] if y else ["-"])
        if va != vb:
            F.add(kind, name, "%s differs at the HUD anchor" % name, x, y, " ".join(va), " ".join(vb), "-", seg)
    x, y = a.get("gui_stack"), b.get("gui_stack")
    sa = ",".join(norm_panel(p) for p in x.f[0].split(",")) if x else "-"
    sb = ",".join(norm_panel(p) for p in y.f[0].split(",")) if y else "-"
    if sa != sb:
        F.add("gui_stack", "stack", "panel stack differs at the HUD anchor", x, y, sa, sb, "-", seg)
    x, y = a.get("sel"), b.get("sel")
    if x and y and tagof(x.f[0]) != tagof(y.f[0]):
        F.add("sel", "target", "selected target differs at the HUD anchor", x, y, x.f[0], y.f[0], "-", seg)


# ----------------------------------------------------------------------------------------------- event classes
def multiset_findings(F, cls, field, ca, cb, label, sr, se, seg, kind="engine", ratio=3.0, floor=10):
    """Rule-level rows from two count tables: keys on one side only, and keys one side uses `ratio` times more often."""
    only_r = sorted(((ca[k], k) for k in ca if k not in cb and ca[k] >= 1), reverse=True)
    only_e = sorted(((cb[k], k) for k in cb if k not in ca and cb[k] >= 1), reverse=True)
    more_r = sorted(((ca[k] - cb[k], k) for k in ca if k in cb and ca[k] >= floor and ca[k] >= ratio * cb[k]), reverse=True)
    more_e = sorted(((cb[k] - ca[k], k) for k in cb if k in ca and cb[k] >= floor and cb[k] >= ratio * ca[k]), reverse=True)
    for rows, text, side in ((only_r, "used by retail only", "r"), (only_e, "used by engine only", "e"),
                             (more_r, "used at least %.0fx more by retail" % ratio, "r"),
                             (more_e, "used at least %.0fx more by engine" % ratio, "e")):
        if not rows:
            continue
        total = sum(n for n, _ in rows)
        keys = ", ".join("%s x%d" % (k, n) for n, k in rows[:8]) + (" ..." if len(rows) > 8 else "")
        F.add(cls, field, "%d %s %s (events in excess: see count)" % (len(rows), label, text), sr.ready if side == "r" else None,
              se.ready if side == "e" else None, keys if side == "r" else "-", keys if side == "e" else "-", str(rows[0][1]), seg, n=total)


def all_rows(seg, kind, op=None):
    return [r for r in seg.recs if r.kind == kind and (op is None or (r.f and r.f[0] == op))]


def rows_of(seg, kind, op=None):
    lim = seg.limit if seg.limit is not None else float("inf")
    return [r for r in seg.recs if r.kind == kind and r.mt <= lim and (op is None or (r.f and r.f[0] == op))]


def snd_family(res):
    base = re.sub(r"\d+$", "", res.lower())
    return re.split(r"[_\d]", base)[0] or base


def cmp_events(sr, se, F, seg):
    # act: action ids
    ca = Counter(r.f[0] for r in rows_of(sr, "act"))
    cb = Counter(r.f[0] for r in rows_of(se, "act"))
    multiset_findings(F, "act", "id", ca, cb, "action ids", sr, se, seg)
    # snd starts by (kind, family)
    def fam(seg_):
        c = Counter()
        for r in rows_of(seg_, "snd", "start"):
            if len(r.f) > 2:
                c[(r.f[1] if r.f[1] == "stream" else "sample") + ":" + snd_family(r.f[2])] += 1
        return c
    multiset_findings(F, "snd", "start", fam(sr), fam(se), "sound families", sr, se, seg)
    # rnd
    def rnd(seg_):
        c = Counter()
        for r in rows_of(seg_, "rnd"):
            c[r.f[0]] += 1
        return c
    multiset_findings(F, "rnd", "events", rnd(sr), rnd(se), "round event kinds", sr, se, seg, floor=1, ratio=2.0)
    ow_r = Counter(tagof(r.f[2]) for r in rows_of(sr, "rnd", "start") if len(r.f) > 2)
    ow_e = Counter(tagof(r.f[2]) for r in rows_of(se, "rnd", "start") if len(r.f) > 2)
    only_r = [k for k in ow_r if k not in ow_e]
    only_e = [k for k in ow_e if k not in ow_r]
    if only_r or only_e:
        F.add("rnd", "owner", "round owners differ (retail only: %s; engine only: %s)" % (", ".join(sorted(only_r)[:6]) or "-",
              ", ".join(sorted(only_e)[:6]) or "-"), sr.ready, se.ready, str(len(only_r)), str(len(only_e)), "-", seg,
              n=len(only_r) + len(only_e))
    # gui open
    def gopen(seg_):
        c = Counter()
        for r in rows_of(seg_, "gui", "open"):
            if norm_panel(r.f[1]) not in ("loadscreen", "saveload", "mainmenu"):
                c[norm_panel(r.f[1])] += 1
        return c
    multiset_findings(F, "gui", "open", gopen(sr), gopen(se), "panels opened", sr, se, seg, floor=1, ratio=2.0)
    # ctl
    def ctl(seg_):
        d = defaultdict(dict)
        for r in rows_of(seg_, "ctl"):
            if len(r.f) >= 6 and r.f[1].lstrip("-").isdigit() and int(r.f[1]) >= 0:
                d[norm_panel(r.f[0])][int(r.f[1])] = tuple(r.f[2:6])
        return d
    cr, ce = ctl(sr), ctl(se)
    for pn in sorted(set(cr) | set(ce)):
        if pn not in ce or pn not in cr:
            F.add("ctl", "panel", "panel %s has controls on %s only" % (pn, "retail" if pn in cr else "engine"), sr.ready, se.ready,
                  str(len(cr.get(pn, {}))), str(len(ce.get(pn, {}))), pn, seg, n=1)
            continue
        ids = set(cr[pn]) ^ set(ce[pn])
        rect = [i for i in set(cr[pn]) & set(ce[pn]) if cr[pn][i] != ce[pn][i]]
        if ids:
            F.add("ctl", "ids", "panel %s: control ids present on one side only" % pn, sr.ready, se.ready, "ids " + str(sorted(set(cr[pn]) - set(ce[pn]))[:6]),
                  "ids " + str(sorted(set(ce[pn]) - set(cr[pn]))[:6]), pn, seg, n=len(ids))
        if rect:
            i0 = sorted(rect)[0]
            F.add("ctl", "rect", "panel %s: control rect differs" % pn, sr.ready, se.ready, "id %d %s" % (i0, cr[pn][i0]),
                  "id %d %s" % (i0, ce[pn][i0]), pn, seg, n=len(rect))
    # pause / camm sequences
    for kind, field in (("pause", "value"), ("camm", "mode")):
        map_ = {"restore": "chase", "follow": "chase"} if kind == "camm" else {}
        va = [(r, map_.get(r.f[0].lower(), r.f[0].lower())) for r in ([sr.carry[kind]] if kind in sr.carry and kind == "pause" else []) + rows_of(sr, kind)]
        vb = [(r, map_.get(r.f[0].lower(), r.f[0].lower())) for r in ([se.carry[kind]] if kind in se.carry and kind == "pause" else []) + rows_of(se, kind)]
        # drop consecutive repeats
        def dedupe(v):
            out = []
            for r, x in v:
                if not out or out[-1][1] != x:
                    out.append((r, x))
            return out
        va, vb = dedupe(va), dedupe(vb)
        sa, sb = [x for _, x in va], [x for _, x in vb]
        if sa != sb:
            i = next((k for k in range(min(len(sa), len(sb))) if sa[k] != sb[k]), min(len(sa), len(sb)))
            F.add(kind, field, "%s sequence differs (first at index %d)" % (kind, i), va[i][0] if i < len(va) else None,
                  vb[i][0] if i < len(vb) else None, " ".join(sa[:8]), " ".join(sb[:8]), "-", seg, n=max(len(sa), len(sb)) - min(len(sa), len(sb)) or 1)
    # dlg lines: aligned as sequences on (speaker, camId, angle) over the whole segment (a run that is cut short or paced
    # differently must not shift every later line); matched lines are compared field by field
    dr, de = all_rows(sr, "dlg", "line"), all_rows(se, "dlg", "line")
    old = [r for r in dr + de if len(r.f) < 10]
    if old:
        NOTES.append("%s: %d dlg rows in the old format skipped" % (seg, len(old)))
        dr = [r for r in dr if len(r.f) >= 10]
        de = [r for r in de if len(r.f) >= 10]
    names = ["owner", "speaker", "listener", "angle", "camId", "nAnims", "textHash"]

    def lkey(r):
        return (tagof(r.f[4]), r.f[7], r.f[6])
    sm = difflib.SequenceMatcher(None, [lkey(r) for r in dr], [lkey(r) for r in de], autojunk=False)
    pairs = []
    for tag_, i1, i2, j1, j2 in sm.get_opcodes():
        if tag_ == "equal":
            pairs += list(zip(range(i1, i2), range(j1, j2)))
        elif tag_ == "replace":
            n = min(i2 - i1, j2 - j1)
            pairs += list(zip(range(i1, i1 + n), range(j1, j1 + n)))
            if i2 - i1 > n:
                lines = dr[i1 + n:i2]
                F.add("dlg", "line", "dialogue lines on retail with no counterpart on the engine (speaker, camId, angle)", lines[0], None,
                      "line %d: %s" % (i1 + n + 1, lkey(lines[0])), "-", tagof(lines[0].f[3]), seg, n=len(lines))
            if j2 - j1 > n:
                lines = de[j1 + n:j2]
                F.add("dlg", "line", "dialogue lines on the engine with no counterpart on retail (speaker, camId, angle)", None, lines[0],
                      "-", "line %d: %s" % (j1 + n + 1, lkey(lines[0])), tagof(lines[0].f[3]), seg, n=len(lines))
        elif tag_ == "delete":
            lines = dr[i1:i2]
            F.add("dlg", "line", "dialogue lines on retail with no counterpart on the engine (speaker, camId, angle)", lines[0], None,
                  "line %d: %s" % (i1 + 1, lkey(lines[0])), "-", tagof(lines[0].f[3]), seg, n=len(lines))
        elif tag_ == "insert":
            lines = de[j1:j2]
            F.add("dlg", "line", "dialogue lines on the engine with no counterpart on retail (speaker, camId, angle): %s" % (
                  "blank text" if lines[0].f[9] == "b1c5b28d" else "text " + lines[0].f[9]),
                  None, lines[0], "-", "line %d: %s angle %s nAnims %s hash %s" % (j1 + 1, lkey(lines[0]), lines[0].f[6], lines[0].f[8],
                                                                                lines[0].f[9]), tagof(lines[0].f[3]), seg, n=len(lines))
    for i, j in pairs:
        a, b = dr[i].f, de[j].f
        for k, nm in enumerate(names):
            va, vb = a[3 + k], b[3 + k]
            if nm in ("owner", "speaker", "listener"):
                va = "none" if va.lower() == "none" else ("pc" if tagof(va) == "pc" else tagof(va))
                vb = "none" if vb.lower() == "none" else ("pc" if tagof(vb) == "pc" else tagof(vb))
            if nm == "textHash" and ("-" in (va, vb)):
                continue
            if va != vb:
                small = nm in ("owner", "speaker", "listener") and va in ("none", "pc") and vb in ("none", "pc")
                rule = "dialogue line %s differs: retail %s, engine %s" % (nm, va, vb) if (small or nm in ("angle", "camId", "nAnims"))                     else "dialogue line %s differs" % nm
                if nm in ("angle", "camId", "nAnims"):
                    rule = "dialogue line %s differs (e.g. retail %s, engine %s)" % (nm, va, vb)
                F.add("dlg", nm, rule, dr[i], de[j], "line %d: %s" % (i + 1, a[3 + k]), "line %d: %s" % (j + 1, b[3 + k]),
                      tagof(a[4]), seg)
    # dlgcam eye (angle, side) sequence, aligned
    er, ee_ = all_rows(sr, "dlgcam", "eye"), all_rows(se, "dlgcam", "eye")
    sm2 = difflib.SequenceMatcher(None, [(r.f[1], r.f[2]) for r in er], [(r.f[1], r.f[2]) for r in ee_], autojunk=False)
    for tag_, i1, i2, j1, j2 in sm2.get_opcodes():
        if tag_ == "equal":
            continue
        F.add("dlgcam", "eye", "dialogue camera eye rows (angle, side) %s" % (
              "only on retail" if j1 == j2 else ("only on the engine" if i1 == i2 else "differ")),
              er[i1] if i1 < len(er) else None, ee_[j1] if j1 < len(ee_) else None,
              str([(r.f[1], r.f[2]) for r in er[i1:i2]][:5]), str([(r.f[1], r.f[2]) for r in ee_[j1:j2]][:5]), "-", seg,
              n=max(i2 - i1, j2 - j1))
    # timing of the first dialogue line / round / HUD
    first_pair = ([dr[pairs[0][0]]], [de[pairs[0][1]]]) if pairs else ([], [])
    for label, ra, rb in (("first matched dialogue line", first_pair[0], first_pair[1]), ("first round start", rows_of(sr, "rnd", "start")[:1],
                                                                  rows_of(se, "rnd", "start")[:1])):
        if ra and rb:
            ta, tb = ra[0].mt / 1000.0, rb[0].mt / 1000.0
            if abs(ta - tb) > TOL["time_s"]:
                F.add("timing", label, "%s happens at a different module time (%.1f s retail, %.1f s engine)" % (label, ta, tb), ra[0], rb[0],
                      "%.1f s" % ta, "%.1f s" % tb, "-", seg, kind="state")
        elif ra or rb:
            F.add("timing", label, "%s happens on %s only" % (label, "retail" if ra else "engine"), (ra or [None])[0], (rb or [None])[0],
                  "-", "-", "-", seg)


# ----------------------------------------------------------------------------------------------- driver
def run(retail, engine, sample, out, top):
    rr, mr = load(retail)
    ee, me = load(engine)
    pre_r, sr = segments(rr)
    pre_e, se = segments(ee)
    F = Findings()
    ledger = []
    emap = {(s.module, s.index): s for s in se}
    rmap = {(s.module, s.index): s for s in sr}
    checked = 0
    limits = {}
    for s in sr:
        e = emap.get((s.module, s.index))
        label = "%s#%d" % (s.module, s.index)
        if e is None:
            F.add("mod", "segment", "module %s ran on retail only" % s.module, s.ready, None, s.module, "-", s.module, label)
            continue
        s.limit = e.limit = min(s.end, e.end)
        limits[label] = s.limit
        ledger.append((label, len(s.recs), len(e.recs)))
        checked += cmp_objects(s, e, F, label)
        cmp_state(s, e, F, label)
        cmp_events(s, e, F, label)
    for s in se:
        if (s.module, s.index) not in rmap:
            F.add("mod", "segment", "module %s ran on engine only" % s.module, None, s.ready, "-", s.module, s.module,
                  "%s#%d" % (s.module, s.index))
    rows = sorted(F.rows.values(), key=lambda r: -r["count"])
    os.makedirs(out, exist_ok=True)
    cols = ["rank", "class", "field", "rule", "count", "first_frame_retail", "first_frame_engine", "module_time_ms",
            "example_retail", "example_engine", "tag", "segment", "verdict", "owner"]
    with open(os.path.join(out, "DIFF.tsv"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\t".join(cols) + "\n")
        for i, r in enumerate(rows, 1):
            verdict, owner = classify(r)
            fh.write("\t".join(str(x).replace("\t", " ").replace("\n", " ") for x in (
                i, r["class"], r["field"], r["rule"], r["count"], r["frame_r"], r["frame_e"], r["time_ms"], clip(r["ex_r"], 160),
                clip(r["ex_e"], 160), r["tag"], r["segment"], verdict, owner)) + "\n")
    lines = ["sample %s: retail %s (%d rows) vs engine %s (%d rows)" % (sample, os.path.basename(os.path.dirname(retail)) or retail, len(rr),
                                                                       os.path.basename(engine), len(ee)),
             "load segment (not compared): retail %d rows, engine %d rows; segments: %s; objects paired: %d" % (
                 len(pre_r), len(pre_e), ", ".join("%s (%d/%d rows, events up to %.1f s)" % (x + (limits.get(x[0], 0) / 1000.0,))
                                                  for x in ledger) or "none shared", checked),
             "%d divergence classes (full list in %s)" % (len(rows), os.path.join(out, "DIFF.tsv"))]
    for i, r in enumerate(rows[:top], 1):
        verdict, owner = classify(r)
        lines.append("%2d. %-9s %-12s n=%-5s %s | %s" % (i, r["class"], r["field"], r["count"], clip(r["rule"], 110), verdict))
    for n in NOTES:
        lines.append("note: " + n)
    text = "\n".join(lines)
    open(os.path.join(out, "SUMMARY.txt"), "w", encoding="utf-8").write(text + "\n")
    print(text)
    return 0


# (class, field, rule substring or None, verdict, owner / note). First match wins. Verdicts:
#   real engine difference | recorder/emitter mismatch | state mismatch (the two runs were not in the same state)
# `owner` is the engine/src file most likely to hold the fix (paths relative to engine/src).
VERDICTS = [
    ("act", "id", "3x more by engine", "real engine difference",
     "game/ScriptHost.cpp, game/ScriptActions.cpp, game/ScriptEvents.cpp (script polling cadence: heartbeat / DelayCommand / AI loops run far more often)"),
    ("act", "id", "3x more by retail", "real engine difference",
     "game/ScriptCombat.cpp, game/ScriptHost.cpp (retail runs the combat-round and AI scripts of every creature; the engine only for attackers)"),
    ("act", "id", "retail only", "real engine difference", "game/ScriptCombat.cpp, game/ScriptHost.cpp (combat / perception / talent actions never run)"),
    ("act", "id", "engine only", "real engine difference", "game/ScriptHost.cpp (engine-only action traffic: local-variable and waypoint polling)"),
    ("snd", "start", "retail", "real engine difference",
     "game/GameSession.cpp ~2956 (footsteps come from clip snd_Footstep events and AudioSystem::playFootstep is only called by tests), game/AudioSystem.cpp (combat sounds)"),
    ("snd", "start", "engine", "real engine difference", "game/AudioSystem.cpp (ambient / music one-shots retail does not play)"),
    ("obj", "template", None, "recorder/emitter mismatch",
     "retail template field reads `-` or a stub (`po_`) for save-loaded objects; compare by tag + position instead"),
    ("obj", "hp", "placeable", "real engine difference", "kotor/Entities.cpp (UTP HP / CurrentHP not loaded; hp stays 1), kotor/SaveGame.cpp:661"),
    ("obj", "hp", "door", "real engine difference", "kotor/Entities.cpp ~1540 (UTD HP is applied on the GIT path, not for save-restored doors), kotor/SaveGame.cpp:661"),
    ("obj", "hp", "creature", "real engine difference", "kotor/SaveGame.cpp, kotor/Entities.cpp ~1255 (current / max hit points of save-restored creatures)"),
    ("obj", "pos", None, "real engine difference", "kotor/SaveGame.cpp (a saved creature, e.g. Carth, is restored at its GIT spot instead of its saved position)"),
    ("obj", "facing", None, "real engine difference", "kotor/SaveGame.cpp (saved creature orientation not applied; the GIT facing is kept)"),
    ("obj", "pairing", None, "real engine difference", "kotor/SaveGame.cpp (see obj pos)"),
    ("obj", "unpaired", "engine only", "recorder/emitter mismatch", "game/StateTrace.cpp (engine-internal stunt actors; filtered by model tag)"),
    ("obj", "unpaired", "retail only", "real engine difference", "kotor/Entities.cpp / kotor/SaveGame.cpp (object the engine does not spawn or hides)"),
    ("ctl", "ids", None, "recorder/emitter mismatch",
     "mipc210x7.gui has 108 top-level controls and the engine-only ids (5, 15, 20, 21, 32, 59 ...) are all CONTROLTYPE 4 labels; retail lists 83-99, so its control array omits some labels (rule not identified; the engine list matches the layout)"),
    ("ctl", "rect", None, "real engine difference", "game/Hud.cpp (retail resizes the control at runtime, e.g. id 8 height 0; the engine keeps the layout rect)"),
    ("ctl", "panel", None, "real engine difference", "app/Application.cpp (the engine's fade is not a GUI panel with controls)"),
    ("gui", "open", None, "real engine difference",
     "app/Application.cpp, game/GuiSystem.cpp (letterbox bars and fades are not manager panels in the engine; retail opens anon@ panels and several fades)"),
    ("gui_stack", "stack", None, "real engine difference", "app/Application.cpp (same cause as gui open; Endar also follows from the blank dialogue nodes)"),
    ("rnd", "events", "retail only", "recorder/emitter mismatch", "game/StateTrace.cpp (the engine has no round-end event to write)"),
    ("rnd", "events", None, "real engine difference", "game/ScriptCombat.cpp (retail starts a round for every creature about every 3 s, in or out of combat)"),
    ("rnd", "owner", None, "real engine difference", "game/ScriptCombat.cpp (round owners)"),
    ("dlg", "line", "blank text", "real engine difference",
     "game/GameSession.cpp showDialogueEntry (blank-text animation nodes open the dialogue UI and count as lines; retail runs them without HandleDialogEntry)"),
    ("dlg", "line", "retail with no counterpart", "real engine difference", "game/GameSession.cpp (a zero-length node the engine merges or skips)"),
    ("dlg", "listener", None, "recorder/emitter mismatch", "game/StateTrace.cpp (listener `none` when the DLG entry names no Listener; fixed after this run)"),
    ("dlg", None, None, "real engine difference", "game/GameSession.cpp, game/DialogueShots.h"),
    ("dlgcam", "eye", None, "recorder/emitter mismatch",
     "game/StateTrace.cpp (no eye rows while the stunt camera model plays; retail logs every eye change)"),
    ("cam", "clip", None, "real engine difference", "render/SceneView.h, game/GameSession.cpp camera() (far plane 500 vs 10000)"),
    ("cam", None, None, "real engine difference",
     "game/GameSession.cpp camera() (initial follow-camera yaw / pitch / distance after a save load; in Endar the dialogue camera is already up)"),
    ("camm", None, None, "real engine difference", "game/GameSession.cpp (camera mode order; Endar follows from the blank dialogue nodes)"),
    ("sel", "target", None, "state mismatch", "the engine selects the object under the pointer left by the menu clicks; retail had none selected"),
    ("pause", None, None, "real engine difference", "app/Application.cpp (autopause timing: enemy-sighted pause fires at a different time)"),
    ("timing", None, "dialogue", "real engine difference", "game/GameSession.cpp (conversation start time)"),
    ("timing", None, "round", "real engine difference", "game/ScriptCombat.cpp (first combat round)"),
    ("obj", "appearance", None, "real engine difference",
     "kotor/Entities.cpp, Chargen (PC appearance after chargen with the same picks: retail 160, engine 136; stunt actors such as Bastila read 375 on retail, 4 on the engine: retail runs module scripts that change appearance)"),
    ("obj", "clip", None, "real engine difference",
     "game/Animation*.cpp (idle clip choice: retail starts the walk / cwalk loop of ambient creatures, the engine a pause1 / cpause1 idle)"),
    ("obj", "visible", None, "real engine difference", "game/GameSession.cpp (minigame modules: the PC is visible on retail, hidden on the engine)"),
    ("party", None, None, "real engine difference",
     "game/GameSession.cpp (party table at the HUD anchor: retail has none until the module settles, the engine lists the PC at once)"),
    ("shot", "anchor", None, "recorder/emitter mismatch", "game/StateTrace.cpp (no HUD panel opened in that module, so no anchor shot)"),
    ("gui", "open", "pause", "real engine difference", "app/Application.cpp (pause panel opened on one side only: enemy-sighted autopause)"),
    ("ctl", "panel", "pause", "real engine difference", "app/Application.cpp (the pause panel and its controls exist on one side only)"),
    ("ctl", "panel", "loadscreen", "real engine difference", "app/Application.cpp (the engine loading screen is not a GUI panel with controls)"),
    ("mod", None, None, "state mismatch", "module sequence differs between the two runs"),
]


def classify(r):
    for cls, field, sub, verdict, owner in VERDICTS:
        if r["class"] == cls and (field is None or r["field"] == field) and (sub is None or sub in r["rule"]):
            return verdict, owner
    return "unclassified", "-"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("retail")
    ap.add_argument("engine")
    ap.add_argument("--sample", default="sample")
    ap.add_argument("--out", default="")
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(__file__)), "diff", a.sample)
    return run(a.retail, a.engine, a.sample, out, a.top)


if __name__ == "__main__":
    sys.exit(main())
