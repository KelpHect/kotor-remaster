#!/usr/bin/env python3
"""Lane s1-06-dynamic: drive ONE owned instance of the windowed 1280x720 patched
swkotor.exe copy in the lane profile, instrumented by sprint1_hooks.js, and execute
a JSON scenario of UI steps over a loopback control socket.

Safety envelope (enforced here):
  - install read-only: sentinel SHA-256 check before AND after the run; drift = stop
  - consent gate: artifacts/lanes/s1-06-dynamic/CONSENT must exist and --consent passed
  - one owned instance: refuses if any swkotor.exe is already running; stops the
    instance it spawned when the scenario ends
  - profile-local only: the patched COPY under artifacts/lanes/s1-06-dynamic/profile;
    the install exe is never patched or written
  - capture only the identified game window (class "Render Window" of the owned pid)
  - fresh save slots only; original saves are never overwritten

Usage:
  python tools/harness/sprint1_dynamic_run.py --label <run-label> \
      --scenario <scenario.json> --consent [--mesa-source <dir-with-mesa-dlls>]
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sprint1_probes as sp  # noqa: E402

REPO = sp.REPO_ROOT
LANE = sp.LANE
# SP1_PROFILE lets a hardware-driver run use a profile copy without Mesa DLLs;
# SP1_HARDWARE=1 suppresses the llvmpipe environment overrides.
PROFILE = Path(os.environ.get("SP1_PROFILE", str(LANE / "profile")))
HOOK = Path(os.environ.get("SP1_HOOK", str(HERE / "sprint1_hooks.js")))
RUNS = LANE / "runs"

ENV_EXTRA = ({} if os.environ.get("SP1_HARDWARE") == "1" else
             {"GALLIUM_DRIVER": "llvmpipe", "MESA_LOADER_DRIVER_OVERRIDE": "llvmpipe"})
MESA_DLLS = ("opengl32.dll", "libgallium_wgl.dll", "dxil.dll")


def sha(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tree(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): sha(p) for p in sorted(root.rglob("*")) if p.is_file()}


u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32
u32.SetProcessDPIAware()


class RECT(ctypes.Structure):
    _fields_ = [("left", wt.LONG), ("top", wt.LONG), ("right", wt.LONG), ("bottom", wt.LONG)]


EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u32.EnumWindows.argtypes = [EnumProc, wt.LPARAM]
u32.EnumWindows.restype = wt.BOOL
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetWindowThreadProcessId.restype = wt.DWORD
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.IsWindowVisible.restype = wt.BOOL
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetClassNameW.restype = ctypes.c_int
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(RECT)]
u32.GetWindowRect.restype = wt.BOOL
u32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
u32.PrintWindow.restype = wt.BOOL
u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(RECT)]
u32.GetClientRect.restype = wt.BOOL
u32.SetForegroundWindow.argtypes = [wt.HWND]
u32.SetForegroundWindow.restype = wt.HWND
u32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
u32.BringWindowToTop.argtypes = [wt.HWND]
u32.SetActiveWindow.argtypes = [wt.HWND]
u32.SetActiveWindow.restype = wt.HWND
u32.SetFocus.argtypes = [wt.HWND]
u32.SetFocus.restype = wt.HWND
k32.GetCurrentThreadId.restype = wt.DWORD
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
k32.GetExitCodeProcess.restype = wt.BOOL
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.CloseHandle.restype = wt.BOOL
u32.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
u32.AttachThreadInput.restype = wt.BOOL
k32.SetThreadExecutionState.argtypes = [wt.DWORD]
k32.SetThreadExecutionState.restype = wt.DWORD


def game_windows(pid: int) -> list[dict]:
    out = []

    @EnumProc
    def cb(hwnd, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value != pid:
            return True
        buf = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(hwnd, buf, 64)
        r = RECT()
        u32.GetWindowRect(hwnd, ctypes.byref(r))
        out.append({"hwnd": hwnd, "cls": buf.value, "visible": bool(u32.IsWindowVisible(hwnd)),
                    "rect": [r.left, r.top, r.right, r.bottom]})
        return True

    u32.EnumWindows(cb, 0)
    return out


def render_window(pid: int) -> dict | None:
    ws = [w for w in game_windows(pid) if w["cls"] == "Render Window" and w["visible"]]
    ws.sort(key=lambda w: (w["rect"][2] - w["rect"][0]) * (w["rect"][3] - w["rect"][1]), reverse=True)
    return ws[0] if ws else None


def focus_once(pid: int) -> dict:
    w = render_window(pid)
    if not w:
        return {"ok": False, "why": "no render window"}
    h = w["hwnd"]
    tid_target = u32.GetWindowThreadProcessId(h, None)
    fg = u32.GetForegroundWindow()
    tid_fg = u32.GetWindowThreadProcessId(fg, None) if fg else 0
    tid_cur = k32.GetCurrentThreadId()
    attached = []
    for tid in {tid_target, tid_fg}:
        if tid and tid != tid_cur:
            if u32.AttachThreadInput(tid_cur, tid, True):
                attached.append(tid)
    u32.ShowWindow(h, 9)
    u32.BringWindowToTop(h)
    u32.SetForegroundWindow(h)
    u32.SetActiveWindow(h)
    u32.SetFocus(h)
    for tid in attached:
        u32.AttachThreadInput(tid_cur, tid, False)
    fg2 = u32.GetForegroundWindow()
    return {"ok": True, "hwnd": int(h), "foreground": int(fg2 or 0), "is_foreground": int(fg2 or 0) == h}


def capture_frame(pid: int, out_path: Path) -> dict:
    """PrintWindow the identified game window only; save BMP; return basic stats."""
    w = render_window(pid)
    if not w:
        return {"ok": False, "why": "no render window"}
    h = w["hwnd"]
    r = RECT()
    u32.GetClientRect(h, ctypes.byref(r))
    cw, ch = r.right - r.left, r.bottom - r.top
    if cw <= 0 or ch <= 0:
        return {"ok": False, "why": "empty client"}
    hdc_win = u32.GetDC(h) if hasattr(u32, "GetDC") else None
    gdi = ctypes.windll.gdi32
    hdc = gdi.CreateCompatibleDC(hdc_win)
    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                    ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]
    class BITMAPINFO(ctypes.Structure):
        _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 1)]
    bm = gdi.CreateCompatibleBitmap(hdc_win, cw, ch)
    gdi.SelectObject(hdc, bm)
    ok = u32.PrintWindow(h, hdc, 2)  # PW_RENDERFULLCONTENT
    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = cw
    bi.bmiHeader.biHeight = -ch  # top-down
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    buf = (ctypes.c_ubyte * (cw * ch * 4))()
    gdi.GetDIBits(hdc, bm, 0, ch, buf, ctypes.byref(bi), 0)
    gdi.DeleteObject(bm)
    gdi.DeleteDC(hdc)
    if hdc_win:
        u32.ReleaseDC(h, hdc_win)
    raw = bytes(buf)
    # BMP file header + info header + pixels (32bpp top-down -> negative height)
    row = cw * 4
    img_size = row * ch
    fh = struct_pack_bmp(cw, ch, img_size)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(fh + raw)
    # stats: mean brightness of a sparse sample
    sample = raw[::4096]
    mean = sum(sample) / max(1, len(sample))
    return {"ok": bool(ok), "hwnd": int(h), "w": cw, "h": ch, "bytes": len(raw),
            "mean_b": round(mean, 2), "path": str(out_path)}


def struct_pack_bmp(cw: int, ch: int, img_size: int) -> bytes:
    import struct
    fh = struct.pack("<2sIHHI", b"BM", 14 + 40 + img_size, 0, 0, 14 + 40)
    ih = struct.pack("<IiiHHIIiiII", 40, cw, -ch, 1, 32, 0, img_size, 0, 0, 0, 0)
    return fh + ih




# ---- SendInput (hardware-level) helpers ------------------------------------
class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]

class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]

class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wt.DWORD), ("wParamL", wt.WORD), ("wParamH", wt.WORD)]

class INPUT_UNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]

class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("union", INPUT_UNION)]

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
KEYEVENTF_KEYUP = 0x0002

u32.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
u32.SendInput.restype = wt.UINT
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u32.GetCursorPos.argtypes = [ctypes.POINTER(ctypes.wintypes.POINT)]
u32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(ctypes.wintypes.POINT)]


def send_key(vk: int, up: bool, hold: float = 0.06) -> None:
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp.union.ki.wVk = vk
    inp.union.ki.dwFlags = KEYEVENTF_KEYUP if up else 0
    u32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    if not up and hold:
        time.sleep(hold)


MOUSEEVENTF_ABSOLUTE = 0x8000


def hw_click(pid: int, x: int, y: int) -> dict:
    w = render_window(pid)
    if not w:
        return {"ok": False, "why": "no render window"}
    h = w["hwnd"]
    r = RECT()
    u32.GetClientRect(h, ctypes.byref(r))
    cw, ch = r.right - r.left, r.bottom - r.top
    # absolute normalized 0..65535 over the client area
    ax = int(x * 65535 / max(1, cw))
    ay = int(y * 65535 / max(1, ch))
    for flags in (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                  MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE,
                  MOUSEEVENTF_LEFTUP | MOUSEEVENTF_ABSOLUTE):
        inp = INPUT()
        inp.type = INPUT_MOUSE
        inp.union.mi.dx = ax
        inp.union.mi.dy = ay
        inp.union.mi.dwFlags = flags
        u32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.08)
    gp = ctypes.wintypes.POINT()
    u32.GetCursorPos(ctypes.byref(gp))
    return {"ok": True, "hwnd": int(h), "x": x, "y": y, "abs": [ax, ay],
            "cursor_screen": [gp.x, gp.y]}


def preflight(args) -> dict:
    install = sp.game_dir_arg(args)
    problems = []
    for name, expected in sp.SENTINELS.items():
        p = install / name
        got = sha(p) if p.exists() else None
        if got != expected:
            problems.append(f"sentinel drift: {name} {got}")
    if not (LANE / "CONSENT").exists() or not args.consent:
        problems.append("no display consent (CONSENT file + --consent required)")
    procs = sp.game_processes()
    if procs:
        problems.append(f"swkotor.exe already running: {procs}")
    exe = PROFILE / "swkotor.exe"
    if sha(exe) != sp.PATCHED_SHA256:
        problems.append(f"profile exe hash mismatch: {sha(exe)}")
    if problems:
        raise SystemExit("PREFLIGHT FAILED: " + "; ".join(problems))
    return {"install": str(install), "profile_exe": sha(exe), "sentinels_ok": True,
            "consent": True, "game_processes": 0}


class Session:
    def __init__(self, run: Path, label: str):
        self.run = run
        self.label = label
        self.events_path = run / "events.jsonl"
        self.events_f = None
        self.script = None
        self.session = None
        self.dev = None
        self.pid = None
        self.dead = False

    def launch(self, mesa_source: Path | None):
        import frida
        if mesa_source:
            for dll in MESA_DLLS:
                src = mesa_source / dll
                dst = PROFILE / dll
                if src.exists() and (not dst.exists() or sha(dst) != sha(src)):
                    shutil.copy2(src, dst)
        env = os.environ.copy()
        env.update(ENV_EXTRA)
        self.dev = frida.get_local_device()
        self.pid = self.dev.spawn([str(PROFILE / "swkotor.exe")], cwd=str(PROFILE),
                                  env=env, stdio="pipe")
        self.session = self.dev.attach(self.pid)
        self.detach_info = None

        def on_detached(reason, crash):
            self.detach_info = {"reason": str(reason), "crash": str(crash)}
            try:
                self.events_f.write(json.dumps({"f": "session_detached", **self.detach_info}) + "\n")
            except Exception:
                pass

        self.session.on("detached", on_detached)
        self.events_f = self.events_path.open("w", encoding="utf-8", buffering=1)

        def on_message(msg, data):
            if msg.get("type") == "send":
                self.events_f.write(json.dumps(msg["payload"], ensure_ascii=False) + "\n")
            elif msg.get("type") == "error":
                self.events_f.write(json.dumps({"f": "script_error", "error": str(msg)}) + "\n")

        self.script = self.session.create_script(HOOK.read_text(encoding="utf-8"))
        self.script.on("message", on_message)
        self.script.load()
        self.dev.resume(self.pid)
        return {"pid": self.pid, "resumed": True}

    def rpc(self, fn: str, *a):
        return getattr(self.script.exports_sync, fn)(*a)

    def stop(self):
        try:
            if self.script:
                self.script.unload()
        except Exception:
            pass
        try:
            if self.session:
                self.session.detach()
        except Exception:
            pass
        try:
            if self.dev and self.pid:
                self.dev.kill(self.pid)
        except Exception:
            pass
        if self.events_f:
            self.events_f.close()


def serve_control(sess: Session, run: Path, steps_out: list) -> int:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(4)
    port = srv.getsockname()[1]
    (run / "control-port.txt").write_text(str(port), encoding="ascii")
    stop_flag = {"quit": False}

    def handle(cmd: dict) -> dict:
        op = cmd.get("op")
        if op == "status":
            return {"ok": True, "pid": sess.pid, **sess.rpc("stats"),
                    "windows": game_windows(sess.pid)}
        if op == "wait":
            time.sleep(float(cmd.get("s", 1)))
            return {"ok": True, "waited_s": float(cmd.get("s", 1))}
        if op == "hwnd":
            return {"ok": True, **sess.rpc("hwnd")}
        if op == "focus":
            return {"ok": True, **focus_once(sess.pid)}
        if op == "focusinproc":
            r = sess.rpc("focusinproc")
            time.sleep(0.8)
            return {"ok": True, "result": r, "after": sess.rpc("focusResult")}
        if op == "clickfast":
            return {"ok": True, **sess.rpc("clickfast", int(cmd["x"]), int(cmd["y"]))}
        if op == "move":
            return {"ok": True, **sess.rpc("move", int(cmd["x"]), int(cmd["y"]))}
        if op == "key":
            return {"ok": True, **sess.rpc("key", int(cmd["vk"]))}
        if op == "hwkey":
            vk = int(cmd["vk"])
            hold = float(cmd.get("hold", 0.08))
            send_key(vk, False, hold)
            send_key(vk, True, 0.0)
            time.sleep(0.2)
            return {"ok": True, "vk": vk, "method": "SendInput"}
        if op == "hwclick":
            return hw_click(sess.pid, int(cmd["x"]), int(cmd["y"]))
        if op == "frame":
            name = str(cmd.get("name", "frame"))
            return {"ok": True, "frame": capture_frame(sess.pid, run / "frames" / (name + ".bmp"))}
        if op == "events_tail":
            n = int(cmd.get("n", 20))
            lines = sess.events_path.read_text(encoding="utf-8", errors="replace").splitlines()
            return {"ok": True, "lines": lines[-n:]}
        if op == "interactive":
            # nested accept loop: lets an external client drive the session live
            deadline = time.time() + float(cmd.get("s", 300))
            n = 0
            while time.time() < deadline and not stop_flag["quit"]:
                try:
                    srv.settimeout(max(0.1, deadline - time.time()))
                    conn, _ = srv.accept()
                except socket.timeout:
                    continue
                with conn:
                    buf = b""
                    while not buf.endswith(b"\n"):
                        chunk = conn.recv(65536)
                        if not chunk:
                            break
                        buf += chunk
                    if not buf.strip():
                        continue
                    try:
                        res = handle(json.loads(buf.decode("utf-8")))
                    except Exception as exc:
                        res = {"ok": False, "error": repr(exc)}
                    conn.sendall((json.dumps(res, ensure_ascii=False) + "\n").encode())
                    n += 1
            return {"ok": True, "interactive_commands": n}
        if op == "quit":
            stop_flag["quit"] = True
            return {"ok": True, "quitting": True}
        return {"ok": False, "error": f"unknown op {op!r}"}

    def server():
        while not stop_flag["quit"]:
            try:
                srv.settimeout(1.0)
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            with conn:
                buf = b""
                while not buf.endswith(b"\n"):
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
                if not buf.strip():
                    continue
                try:
                    res = handle(json.loads(buf.decode("utf-8")))
                except Exception as exc:
                    res = {"ok": False, "error": repr(exc)}
                conn.sendall((json.dumps(res, ensure_ascii=False) + "\n").encode())
        srv.close()

    t = threading.Thread(target=server, daemon=True)
    t.start()
    return port


def send_cmd(port: int, cmd: dict, timeout: float = 240) -> dict:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall((json.dumps(cmd) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.decode("utf-8", "replace"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--label", required=True)
    ap.add_argument("--scenario", required=True, help="JSON file with {'steps': [...]}")
    ap.add_argument("--consent", action="store_true")
    ap.add_argument("--mesa-source", default=None,
                    help="dir with Mesa opengl32.dll/libgallium_wgl.dll/dxil.dll to copy into the profile")
    ap.add_argument("--game-dir", default=None)
    ap.add_argument("--runs-dir", default=None)
    args = ap.parse_args()

    pre = preflight(args)
    runs_root = Path(args.runs_dir) if args.runs_dir else RUNS
    run = runs_root / args.label
    if run.exists():
        print(f"RUN EXISTS {run}", file=sys.stderr)
        return 2
    run.mkdir(parents=True)
    (run / "preflight.json").write_text(json.dumps(pre, indent=2) + "\n", encoding="utf-8")
    saves_before = tree(PROFILE / "saves")
    (run / "saves-before.json").write_text(json.dumps(saves_before, indent=1) + "\n", encoding="utf-8")
    k32.SetThreadExecutionState(0x80000003)  # ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED

    scenario = json.loads(Path(args.scenario).read_text(encoding="utf-8"))
    sess = Session(run, args.label)
    steps_out: list[dict] = []
    post_ok = True
    try:
        launch_info = sess.launch(Path(args.mesa_source) if args.mesa_source else None)
        (run / "launch.json").write_text(json.dumps(launch_info, indent=2) + "\n", encoding="utf-8")
        print("launched", launch_info, flush=True)
        port = serve_control(sess, run, steps_out)
        print("control port", port, flush=True)
        for step in scenario.get("steps", []):
            name = step.get("name", "?")
            t0 = time.time()
            try:
                res = send_cmd(port, step)
            except Exception as exc:
                res = {"ok": False, "error": repr(exc)}
            rec = {"step": name, "cmd": step, "elapsed": round(time.time() - t0, 2), "result": res}
            steps_out.append(rec)
            (run / "orchestrator.json").write_text(
                json.dumps(steps_out, indent=1, sort_keys=True) + "\n", encoding="utf-8")
            brief = {k: res.get(k) for k in ("ok", "error", "waited_s", "frame", "hwnd", "fg",
                                             "is_foreground", "seq", "x", "y", "vk", "w", "h")
                     if k in res}
            print(name, json.dumps(brief)[:400], flush=True)
            if name == "quit":
                break
    finally:
        # wait for the game to exit after quit, then hard-stop the owned instance
        deadline = time.time() + 60
        while time.time() < deadline:
            if not sp.game_processes():
                break
            time.sleep(1.0)
        sess.stop()
        k32.SetThreadExecutionState(0x80000000)
        post = {n: (sha(sp.game_dir_arg(args) / n) if (sp.game_dir_arg(args) / n).exists() else None)
                for n in sp.SENTINELS}
        post_ok = all(post[n] == exp for n, exp in sp.SENTINELS.items())
        saves_after = tree(PROFILE / "saves")
        exit_code = None
        try:
            ph = k32.OpenProcess(0x1010, False, sess.pid)  # PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ
            if ph:
                ec = wt.DWORD()
                k32.GetExitCodeProcess(ph, ctypes.byref(ec))
                exit_code = ec.value
                k32.CloseHandle(ph)
        except Exception as exc:
            exit_code = f"err {exc!r}"
        summary = {
            "label": args.label,
            "pid": sess.pid,
            "detach_info": getattr(sess, "detach_info", None),
            "exit_code": exit_code,
            "steps": len(steps_out),
            "install_sentinels_after": post,
            "install_ok_after": post_ok,
            "game_processes_after": sp.game_processes(),
            "new_save_files": sorted(set(saves_after) - set(saves_before)),
            "changed_save_files": sorted(k for k in set(saves_before) & set(saves_after)
                                         if saves_before[k] != saves_after[k]),
        }
        (run / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print("SUMMARY", json.dumps(summary)[:600], flush=True)
    if not post_ok:
        print("STOP: install sentinel drift after run", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
