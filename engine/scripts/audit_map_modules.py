"""Usage: audit_map_modules.py [game dir]. Exit 1 when a module with map art has a missing texture or an entry off the map.

Audit 4: the map of every module, from the shipped data and the engine's transform.

For each of the 117 modules: the ARE's Map struct, the lbl_map<area> texture, and where the module's
entry position lands on the map (Scene::Minimap::project, 440x256 view). Prints one line per module that
is not clean and the counts.
"""
import sys, os, struct, tempfile, glob
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path[:0] = [ROOT + "/tools/resources", ROOT + "/tools/formats", ROOT + "/tools"]
import kotor_vfs as kv, gff
from viewer import tpc

GAME = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("KOTOR_GAME_DIR", "E:/SteamLibrary/steamapps/common/swkotor")
modules = sorted({os.path.basename(p)[:-4].lower() for p in glob.glob(GAME + "/modules/*.rim")
                  if not os.path.basename(p)[:-4].lower().endswith(("_s", "_a", "_adx"))})


def parse(b):
    fd, p = tempfile.mkstemp()
    os.write(fd, b)
    os.close(fd)
    g = gff.Gff(p)
    os.unlink(p)
    return g


def top(g):
    return {f.label: f for f in g.fields_of(0)}


def sv(g, f):
    v = g.value(f)
    return v["__struct__"] if isinstance(v, dict) else v


counts = dict(modules=len(modules), are=0, map_struct=0, res_nonzero=0, texture=0, arrow_in_view=0, arrow_on_art=0,
              res_zero=0)
problems = []
for m in modules:
    v = kv.Vfs(GAME, active_module=m)
    try:
        gi = parse(v.resolve("module", 2014).read_payload())
        fi = top(gi)
        area = str(gi.value(fi["Mod_Entry_Area"])).lower()
        ex = float(gi.value(fi["Mod_Entry_X"]))
        ey = float(gi.value(fi["Mod_Entry_Y"]))
    except Exception as e:
        problems.append((m, "no IFO entry: %s" % e))
        continue
    try:
        ga = parse(v.resolve(area, 2012).read_payload())
    except Exception as e:
        problems.append((m, "no ARE %s: %s" % (area, e)))
        continue
    counts["are"] += 1
    fa = top(ga)
    if "Map" not in fa:
        problems.append((m, "ARE has no Map struct"))
        continue
    counts["map_struct"] += 1
    mp = {ff.label: ga.value(ff) for ff in ga.fields_of(sv(ga, fa["Map"]))}
    resx = int(mp.get("MapResX", 0))
    if resx <= 0:
        counts["res_zero"] += 1
        problems.append((m, "MapResX 0: retail draws no map art for this area (expected empty)"))
        continue
    counts["res_nonzero"] += 1
    try:
        raw = v.resolve("lbl_map" + area, 3007).read_payload()
        img, meta = tpc.decode_texture(raw, 3007)
    except Exception as e:
        problems.append((m, "texture lbl_map%s missing: %s" % (area, e)))
        continue
    counts["texture"] += 1
    W, H = img.w, img.h
    arr = Image.frombytes('RGBA', (W, H), bytes(img.pix)).convert('RGB')
    north = int(mp.get("NorthAxis", 0))
    swap = north >= 2
    w1 = (mp["WorldPt1X"], mp["WorldPt1Y"])
    w2 = (mp["WorldPt2X"], mp["WorldPt2Y"])
    m1 = (mp["MapPt1X"], mp["MapPt1Y"])
    m2 = (mp["MapPt2X"], mp["MapPt2Y"])
    a, b = (ey, ex) if swap else (ex, ey)
    a1, a2 = (w1[1], w2[1]) if swap else (w1[0], w2[0])
    b1, b2 = (w1[0], w2[0]) if swap else (w1[1], w2[1])
    if a2 == a1 or b2 == b1:
        problems.append((m, "degenerate world points"))
        continue
    u = m1[0] + (a - a1) * (m2[0] - m1[0]) / (a2 - a1)
    vv = m1[1] + (b - b1) * (m2[1] - m1[1]) / (b2 - b1)
    px, py = u * 440.0, (1.0 - vv) * 256.0
    if not (0 <= px < 440 and 0 <= py < 256):
        problems.append((m, "entry lands outside the 440x256 view at (%.0f, %.0f)" % (px, py)))
        continue
    counts["arrow_in_view"] += 1
    # the art is drawn 1:1 from the top left of the 512x256 object
    x0, y0 = int(px), int(py)
    patch = arr.crop((max(0, x0 - 16), max(0, y0 - 16), min(W, x0 + 17), min(H, y0 + 17)))
    if patch.size[0] and patch.size[1] and max(ch[1] for ch in patch.getextrema()) > 24:
        counts["arrow_on_art"] += 1
    else:
        problems.append((m, "entry at (%.0f, %.0f) has no drawn map art within 16 texels" % (px, py)))
for p in problems:
    print(p[0], "-", p[1])
print(counts)
sys.exit(1 if any('expected empty' not in p[1] for p in problems) else 0)
