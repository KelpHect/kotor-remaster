'use strict';
// Minimal live-input/file-trace hook.
//
// The full sprint1_hooks.js instruments renderer, camera, clip, mesh and
// thumbnail paths.  On a monitor-off/virtual-display host those extra hooks
// can perturb the engine; this script keeps only the Win32 file-open trace
// needed to observe streamwaves/streammusic/streamsounds requests.  Drive the
// UI with the harness's Python-side focus/hwclick/hwkey operations instead.
const game = Process.getModuleByName('swkotor.exe');
const k32 = Process.getModuleByName('kernel32.dll');
let seq = 0;
let glGetStringCb = null;  // keep the NativeCallback alive for the IAT patch
function emit(o) { o.seq = ++seq; o.t = Date.now(); send(o); }
function cstra(p, max) { try { return p.readAnsiString(max || 512); } catch (e) { return null; } }
function cstrw(p) { try { return p.readUtf16String(1024); } catch (e) { return null; } }

// Mesa returns NULL for GL_PROGRAM_ERROR_STRING_NV (0x8874); the engine
// dereferences it at 0x47a2ce.  Return an empty string for that enum.
try {
  const gl = Process.getModuleByName('opengl32.dll');
  const realGetString = new NativeFunction(gl.getExportByName('glGetString'),
                                           'pointer', ['uint'], 'stdcall');
  const emptyString = Memory.allocUtf8String('');
  glGetStringCb = new NativeCallback(function (which) {
    if ((which >>> 0) === 0x8874) return emptyString;
    try { return realGetString(which); } catch (e) { return ptr(0); }
  }, 'pointer', ['uint'], 'stdcall');
  const cbPtr = ptr(glGetStringCb.toString());
  const iat = game.base.add(0x33d290);  // IAT RVA
  const page = ptr(iat.toUInt32() & 0xfffff000);
  const range = Process.findRangeByAddress(iat);
  const prot = range ? range.protection : 'r--';
  Memory.protect(page, 0x1000, 'rw-');
  iat.writePointer(cbPtr);
  Memory.protect(page, 0x1000, prot);
  emit({ f: 'iat_patch', tag: 'glGetString_0x8874', at: iat.toString(),
         newv: cbPtr.toString() });
} catch (e) {
  emit({ f: 'hookerr', name: 'glGetString_shim', e: String(e) });
}

function hookFile(name, wide) {
  try {
    Interceptor.attach(k32.findExportByName(name), {
      onEnter(args) {
        this.path = wide ? cstrw(args[0]) : cstra(args[0]);
        this.access = args[1].toInt32();
        this.disp = args[4].toInt32();
      },
      onLeave(ret) {
        if (!this.path) return;
        const low = this.path.toLowerCase();
        if (!/(streamwaves|streammusic|streamsounds|\.wav|\.mp3)/.test(low)) return;
        const ok = !ret.isNull() && ret.toInt32() !== -1;
        emit({ f: 'file', api: name, path: this.path, access: this.access,
               disp: this.disp, ok: ok, tid: this.threadId });
      }
    });
  } catch (e) { emit({ f: 'hookerr', name: name, e: String(e) }); }
}
// Mesa's gluBuild2DMipmaps faults on the engine's RGBA path; use
// GL_GENERATE_MIPMAP + glTexImage2D instead (disclosed shim, recon/10/16).
try {
  const gl = Process.getModuleByName('opengl32.dll');
  const glu = Process.getModuleByName('GLU32.dll');
  const texParameteri = gl.getExportByName('glTexParameteri');
  const build = glu.getExportByName('gluBuild2DMipmaps');
  const pTexParam = new NativeFunction(texParameteri, 'void', ['uint', 'uint', 'int'], 'stdcall');
  const repl = new NativeCallback(function (target, internalformat, width, height, format, type, data) {
    try { pTexParam(target, 0x8191, 1); } catch (e) {}
    return 0;
  }, 'int', ['uint', 'int', 'int', 'int', 'uint', 'uint', 'pointer'], 'stdcall');
  Interceptor.replace(build, repl);
  emit({ f: 'hook', target: 'gluBuild2DMipmaps_shim', addr: build.toString() });
} catch (e) { emit({ f: 'hookerr', name: 'glu_shim', e: String(e) }); }

hookFile('CreateFileW', true);
hookFile('CreateFileA', false);

// ---- control: in-process focus + posted input on the identified game window ---
const u32mod = Process.getModuleByName('user32.dll');
function nf(m, n, ret, args) { return new NativeFunction(m.getExportByName(n), ret, args, 'stdcall'); }
const postMessageA = nf(u32mod, 'PostMessageA', 'int', ['pointer', 'uint', 'pointer', 'pointer']);
const showWindow = nf(u32mod, 'ShowWindow', 'int', ['pointer', 'int']);
const bringWindowToTop = nf(u32mod, 'BringWindowToTop', 'int', ['pointer']);
const setForegroundWindow = nf(u32mod, 'SetForegroundWindow', 'pointer', ['pointer']);
const setActiveWindow = nf(u32mod, 'SetActiveWindow', 'pointer', ['pointer']);
const setFocus = nf(u32mod, 'SetFocus', 'pointer', ['pointer']);
const getForegroundWindow = nf(u32mod, 'GetForegroundWindow', 'pointer', []);
const enumWindows = nf(u32mod, 'EnumWindows', 'int', ['pointer', 'pointer']);
const getWindowThreadProcessId = nf(u32mod, 'GetWindowThreadProcessId', 'uint', ['pointer', 'pointer']);
const isWindowVisible = nf(u32mod, 'IsWindowVisible', 'int', ['pointer']);
const getClassNameW = nf(u32mod, 'GetClassNameW', 'int', ['pointer', 'pointer', 'int']);
const getWindowRect = nf(u32mod, 'GetWindowRect', 'int', ['pointer', 'pointer']);
function hs(p) { return p ? p.toString() : null; }

function targetWindow() {
  const pid = Process.id;
  let best = null;
  const cb = new NativeCallback(function (hwnd, lparam) {
    const p = Memory.alloc(4);
    getWindowThreadProcessId(hwnd, p);
    if (p.readU32() !== pid) return 1;
    if (!isWindowVisible(hwnd)) return 1;
    const buf = Memory.alloc(64);
    getClassNameW(hwnd, buf, 64);
    const cls = buf.readUtf16String();
    if (cls !== 'Render Window') return 1;
    const r = Memory.alloc(16);
    getWindowRect(hwnd, r);
    const w = r.add(8).readS32() - r.readS32();
    const h = r.add(12).readS32() - r.add(4).readS32();
    if (!best || w * h > best.area) best = { hwnd: hwnd.toString(), area: w * h, w: w, h: h, cls: cls };
    return 1;
  }, 'int', ['pointer', 'pointer']);
  enumWindows(cb, ptr(0));
  return { found: !!best, hwnd: best ? best.hwnd : null, w: best ? best.w : 0, h: best ? best.h : 0 };
}

function focusGameWindow() {
  const tw = targetWindow();
  if (!tw.found) return { ok: false, why: 'no-target' };
  const h = ptr(tw.hwnd);
  showWindow(h, 9);
  bringWindowToTop(h);
  setForegroundWindow(h);
  setActiveWindow(h);
  setFocus(h);
  postMessageA(h, 0x001C, ptr(1), ptr(0));
  postMessageA(h, 0x0086, ptr(1), ptr(0));
  postMessageA(h, 0x0006, ptr(1), ptr(0));
  postMessageA(h, 0x0007, ptr(0), ptr(0));
  return { ok: true, hwnd: tw.hwnd, w: tw.w, h: tw.h, fg: hs(getForegroundWindow()) };
}

let focusResult = null;
rpc.exports = {
  stats() { return { seq: seq }; },
  ping() { return 'pong'; },
  hwnd() { return targetWindow(); },
  focusinproc() { focusResult = focusGameWindow(); return focusResult; },
  focusResult() { return focusResult; },
  clickfast(x, y) {
    const f = focusGameWindow();
    if (!f.ok) return f;
    const h = ptr(f.hwnd);
    const lp = ptr((((y & 0xffff) << 16) | (x & 0xffff)) >>> 0);
    postMessageA(h, 0x0200, ptr(0), lp);
    postMessageA(h, 0x0201, ptr(1), lp);
    postMessageA(h, 0x0202, ptr(0), lp);
    return { ok: true, hwnd: f.hwnd, x: x, y: y, fg: hs(getForegroundWindow()) };
  },
  move(x, y) {
    const tw = targetWindow();
    if (!tw.found) return { ok: false, why: 'no-target' };
    const h = ptr(tw.hwnd);
    const lp = ptr((((y & 0xffff) << 16) | (x & 0xffff)) >>> 0);
    postMessageA(h, 0x0200, ptr(0), lp);
    return { ok: true, hwnd: tw.hwnd, x: x, y: y };
  },
  key(vk) {
    const tw = targetWindow();
    if (!tw.found) return { ok: false, why: 'no-target' };
    const h = ptr(tw.hwnd);
    postMessageA(h, 0x0100, ptr(vk >>> 0), ptr(0x00000001));
    postMessageA(h, 0x0101, ptr(vk >>> 0), ptr(0xC0000001));
    return { ok: true, hwnd: tw.hwnd, vk: vk };
  },
  text(s) {
    const tw = targetWindow();
    if (!tw.found) return { ok: false, why: 'no-target' };
    const h = ptr(tw.hwnd);
    const str = String(s || '');
    for (let i = 0; i < str.length; i++) {
      const ch = str.charCodeAt(i) & 0xffff;
      postMessageA(h, 0x0102, ptr(ch), ptr(0x00000001));
    }
    return { ok: true, hwnd: tw.hwnd, text: str };
  }
};
emit({ f: 'live_hooks_loaded', pid: Process.id });
