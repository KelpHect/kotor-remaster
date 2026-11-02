'use strict';
// Minimal live-input/file-trace hook.
//
// The full sprint1_hooks.js instruments renderer, camera, clip, mesh and
// thumbnail paths.  On a monitor-off/virtual-display host those extra hooks
// can perturb the engine; this script keeps only the Win32 file-open trace
// needed to observe streamwaves/streammusic/streamsounds plus module/movie
// loads, and the chargen hand-off: it captures the chargen manager in
// FUN_006eb420 and can invoke the real PLAY handler FUN_006eb320 on the game
// thread.  Drive the UI with the harness's Python-side focus/hwclick/hwkey
// operations instead.
const game = Process.getModuleByName('swkotor.exe');
const k32 = Process.getModuleByName('kernel32.dll');
let seq = 0;
try {
  Interceptor.attach(k32.getExportByName('ExitProcess'), {
    onEnter(args) {
      emit({ f: 'exitprocess', code: args[0].toInt32(),
             bt: Thread.backtrace(this.context, Backtracer.ACCURATE).map((x) => x.toString()) });
    }
  });
  Interceptor.attach(k32.getExportByName('TerminateProcess'), {
    onEnter(args) {
      emit({ f: 'terminateprocess', h: args[0].toString(), code: args[1].toInt32(),
             bt: Thread.backtrace(this.context, Backtracer.ACCURATE).map((x) => x.toString()) });
    }
  });
  emit({ f: 'hook', target: 'exit_hooks' });
} catch (e) { emit({ f: 'hookerr', name: 'exit_hooks', e: String(e) }); }

try {
  const ntdll = Process.getModuleByName('ntdll.dll');
  for (const n of ['RtlExitUserProcess', 'NtTerminateProcess', 'ZwTerminateProcess']) {
    try {
      Interceptor.attach(ntdll.getExportByName(n), {
        onEnter(args) {
          const o = { f: 'ntexit', fn: n, a0: args[0].toString(),
                      bt: Thread.backtrace(this.context, Backtracer.ACCURATE).map((x) => x.toString()) };
          if (n !== 'RtlExitUserProcess' && args[1]) o.a1 = args[1].toString();
          emit(o);
        }
      });
      emit({ f: 'hook', target: 'ntexit_' + n });
    } catch (e) { emit({ f: 'hookerr', name: 'ntexit_' + n, e: String(e) }); }
  }
} catch (e) { emit({ f: 'hookerr', name: 'ntdll', e: String(e) }); }

try {
  emit({ f: 'modules', list: Process.enumerateModules().map((m) => ({ name: m.name, base: m.base.toString(), size: m.size })) });
} catch (e) { emit({ f: 'hookerr', name: 'modules', e: String(e) }); }

try {
  const tbl = ptr(0x7a68a4).readPointer();
  const fn = tbl.add(0x14).readPointer();
  const m = Process.findModuleByAddress(fn);
  emit({ f: 'gldraw_ptr', table: tbl.toString(), fn: fn.toString(),
         module: m ? m.name : null, base: m ? m.base.toString() : null });
} catch (e) { emit({ f: 'hookerr', name: 'gldraw_ptr', e: String(e) }); }

Process.setExceptionHandler(function (d) {
  try {
    let mod = null, rng = null;
    try {
      const m = Process.findModuleByAddress(d.address);
      if (m) mod = { name: m.name, base: m.base.toString(), size: m.size };
    } catch (e) {}
    try {
      const r = Process.findRangeByAddress(d.address);
      if (r) rng = { base: r.base.toString(), size: r.size, prot: r.protection,
                     file: r.file ? r.file.path : null };
    } catch (e) {}
    emit({ f: 'exception', type: d.type, address: d.address.toString(),
           module: mod, range: rng,
           bt: Thread.backtrace(d.context, Backtracer.ACCURATE).map((x) => x.toString()) });
  } catch (e) {}
  return false;
});
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
        if (!/(streamwaves|streammusic|streamsounds|\.wav|\.mp3|movies|\.bik|currentgame|modules|\.rim|\.erf|\.mod)/.test(low)) return;
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
const clientToScreen = nf(u32mod, 'ClientToScreen', 'int', ['pointer', 'pointer']);
const setCursorPos = nf(u32mod, 'SetCursorPos', 'int', ['int', 'int']);
const mouseEvent = nf(u32mod, 'mouse_event', 'void', ['uint', 'uint', 'uint', 'uint', 'pointer']);
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
let doPlay = false;
let playFn = null;
let custpnl = null;
let quickPanel = null;
let chargenMgr = null;
let doStart = false;
let startFn = null;

// The visible quick-character panel is FUN_006f0390 (0x1360 bytes); its step
// counter is +0x135c and [panel+0x68] is the chargen manager.  The real PLAY
// action is 0x6efd60 -> FUN_006eb320([panel+0x68]) -> FUN_006dbdf0(manager),
// which starts the module load.
try {
  Interceptor.attach(game.base.add(0x6f0390 - 0x400000), {
    onEnter() { quickPanel = this.context.ecx; },
    onLeave() {
      try {
        const mgr = quickPanel.add(0x68).readPointer();
        emit({ f: 'quickpanel_ready', self: quickPanel.toString(), mgr: mgr.toString(),
               step: quickPanel.add(0x135c).readS32() });
      } catch (e) { emit({ f: 'hookerr', name: 'quickpanel_ready', e: String(e) }); }
    }
  });
  emit({ f: 'hook', target: 'quickpanel_ctor_0x6f0390' });
} catch (e) { emit({ f: 'hookerr', name: 'quickpanel_ctor', e: String(e) }); }

try {
  Interceptor.attach(game.base.add(0x6eb420 - 0x400000), {
    onEnter() {
      chargenMgr = this.context.ecx;
      emit({ f: 'chargen_mgr', mgr: chargenMgr.toString() });
    }
  });
  emit({ f: 'hook', target: 'chargen_mgr_0x6eb420' });
} catch (e) { emit({ f: 'hookerr', name: 'chargen_mgr', e: String(e) }); }

try {
  startFn = new NativeFunction(game.base.add(0x6eb320 - 0x400000), 'void', ['pointer'], 'thiscall');
  const gl = Process.getModuleByName('opengl32.dll');
  Interceptor.attach(gl.getExportByName('wglSwapBuffers'), {
    onEnter() {
      if (!doStart) return;
      doStart = false;
      let mgr = chargenMgr;
      if (quickPanel !== null) {
        try {
          const p = quickPanel.add(0x68).readPointer();
          if (!p.isNull()) mgr = p;
        } catch (e) {}
      }
      if (mgr === null) { emit({ f: 'startgame_error', e: 'no chargen manager captured' }); return; }
      try {
        startFn(mgr);
        emit({ f: 'startgame_called', fn: '0x6eb320', mgr: mgr.toString(), tid: this.threadId });
      } catch (e) { emit({ f: 'startgame_error', e: String(e) }); }
    }
  });
  emit({ f: 'hook', target: 'startgame_0x6eb320' });
} catch (e) { emit({ f: 'hookerr', name: 'startgame', e: String(e) }); }

// CUSTPNL is the chargen step panel (the PORTRAIT/NAME/PLAY screen).
// FUN_006ef730 fills it in; [self+0x68] is the chargen manager and
// [self+0x2184] the current step.
try {
  Interceptor.attach(game.base.add(0x6ef730 - 0x400000), {
    onEnter() { custpnl = this.context.ecx; },
    onLeave() {
      try {
        const mgr = custpnl.add(0x68).readPointer();
        emit({ f: 'custpnl_ready', self: custpnl.toString(), mgr: mgr.toString(),
               step: custpnl.add(0x2184).readS32() });
      } catch (e) { emit({ f: 'hookerr', name: 'custpnl_ready', e: String(e) }); }
    }
  });
  emit({ f: 'hook', target: 'custpnl_ctor_0x6ef730' });
} catch (e) { emit({ f: 'hookerr', name: 'custpnl_ctor', e: String(e) }); }

function probe(tag, rva, kind) {
  try {
    Interceptor.attach(game.base.add(rva - 0x400000), {
      onEnter(args) {
        try {
          const self = this.context.ecx;
          const o = { f: tag, self: self.toString(), tid: this.threadId };
          if (kind === 'event') { o.ev = args[0].toInt32(); o.ctl = args[1].toString(); }
          else if (kind === 'step') { o.step = args[0].toInt32(); }
          emit(o);
        } catch (e) {}
      }
    });
    emit({ f: 'hook', target: tag });
  } catch (e) { emit({ f: 'hookerr', name: tag, e: String(e) }); }
}
probe('custpnl_event', 0x6ef610, 'event');
probe('custpnl_setstep', 0x6eefd0, 'step');
probe('custpnl_playstep', 0x6ef500, 'none');
probe('custpnl_namestep', 0x6ef540, 'none');
probe('custpnl_portraitstep', 0x6ef570, 'none');

try {
  // FUN_006eff80(custpnl): commit the current name and move the step bar to
  // PLAY.  It is the body of the name dialog's END_BTN and needs ECX=CUSTPNL.
  playFn = new NativeFunction(game.base.add(0x6eff80 - 0x400000), 'void', ['pointer'], 'thiscall');
  const gl = Process.getModuleByName('opengl32.dll');
  Interceptor.attach(gl.getExportByName('wglSwapBuffers'), {
    onEnter() {
      if (!doPlay) return;
      doPlay = false;
      if (custpnl === null) { emit({ f: 'direct_play_error', e: 'no custpnl captured' }); return; }
      try { playFn(custpnl); emit({ f: 'direct_play_called', fn: '0x6eff80', self: custpnl.toString(), tid: this.threadId }); }
      catch (e) { emit({ f: 'direct_play_error', e: String(e) }); }
    }
  });
  emit({ f: 'hook', target: 'direct_play_0x6eff80' });
} catch (e) { emit({ f: 'hookerr', name: 'direct_play', e: String(e) }); }

rpc.exports = {
  stats() { return { seq: seq }; },
  ping() { return 'pong'; },
  hwnd() { return targetWindow(); },
  focusinproc() { focusResult = focusGameWindow(); return focusResult; },
  focusResult() { return focusResult; },
  play() {
    if (custpnl === null) return { ok: false, why: 'no custpnl captured' };
    doPlay = true;
    return { ok: true, queued: true, self: custpnl.toString() };
  },
  startgame() {
    let mgr = chargenMgr;
    let step = -1;
    if (quickPanel !== null) {
      try {
        const p = quickPanel.add(0x68).readPointer();
        if (!p.isNull()) mgr = p;
        step = quickPanel.add(0x135c).readS32();
      } catch (e) {}
    }
    if (mgr === null) return { ok: false, why: 'no chargen manager captured' };
    doStart = true;
    return { ok: true, queued: true, mgr: mgr.toString(),
             panel: quickPanel ? quickPanel.toString() : null, step: step };
  },
  hwclick2(x, y) {
    const tw = targetWindow();
    if (!tw.found) return { ok: false, why: 'no-target' };
    const h = ptr(tw.hwnd);
    const pt = Memory.alloc(8);
    pt.writeS32(x);
    pt.add(4).writeS32(y);
    const cts = clientToScreen(h, pt);
    const sx = pt.readS32();
    const sy = pt.add(4).readS32();
    setCursorPos(sx, sy);
    mouseEvent(0x0001, 0, 0, 0, ptr(0));
    mouseEvent(0x0002, 0, 0, 0, ptr(0));
    mouseEvent(0x0004, 0, 0, 0, ptr(0));
    return { ok: true, hwnd: tw.hwnd, x: x, y: y, screen: [sx, sy], cts: cts };
  },
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
