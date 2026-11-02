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
function A(rva) { return game.base.add(rva - 0x400000); }
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

// ---- save-thumbnail shims (Mesa llvmpipe) ---------------------------------
// FUN_00420f20 writes the save screenshot; under Mesa the resampler
// FUN_0045dad0 faults.  Replace the writer with a synthetic 256x256 TGA and
// the resampler with the same nearest-neighbour math as thumb_shim.py.
try {
  const createFileW = new NativeFunction(k32.getExportByName('CreateFileW'), 'pointer',
    ['pointer', 'uint', 'uint', 'pointer', 'uint', 'uint', 'pointer'], 'stdcall');
  const writeFile = new NativeFunction(k32.getExportByName('WriteFile'), 'int',
    ['pointer', 'pointer', 'uint', 'pointer', 'pointer'], 'stdcall');
  const closeHandle = new NativeFunction(k32.getExportByName('CloseHandle'), 'int', ['pointer'], 'stdcall');
  const tga = new Uint8Array(18 + 256 * 256 * 3);
  tga[2] = 2; tga[12] = 0; tga[13] = 1; tga[14] = 0; tga[15] = 1; tga[16] = 24;
  for (let i = 18; i < tga.length; i += 3) { tga[i] = 0x60; tga[i + 1] = 0x60; tga[i + 2] = 0x60; }
  function synthPath(arg) {
    const candidates = [];
    for (const a of [arg, arg.readPointer()]) {
      try { const w = cstrw(a, 260); if (w) candidates.push(w); } catch (e) {}
      try { const c = cstra(a, 260); if (c) candidates.push(c); } catch (e) {}
    }
    if (candidates.length) emit({ f: 'thumb_path_candidates', candidates: candidates });
    for (let s of candidates) {
      if (!/screen(\.tga)?$/i.test(s)) continue;
      if (!/\.tga$/i.test(s)) s += '.tga';
      if (/^SAVES:/i.test(s)) s = '.\\saves\\' + s.slice(6);
      return s;
    }
    return null;
  }
  const repl = new NativeCallback(function (arg) {
    const path = synthPath(arg);
    emit({ f: 'thumb_replace_0x420f20', arg: hs(arg), path: path, tid: this.threadId });
    if (path) {
      try {
        const p = Memory.allocUtf16String(path);
        const h = createFileW(p, 0x40000000, 0, ptr(0), 2, 0, ptr(0));
        if (h && !h.isNull() && h.toInt32() !== -1) {
          const buf = Memory.alloc(tga.length);
          buf.writeByteArray(tga);
          const written = Memory.alloc(4);
          writeFile(h, buf, tga.length, written, ptr(0));
          closeHandle(h);
          emit({ f: 'screen_tga_synth', path: path, bytes: tga.length, written: written.readU32(), tid: this.threadId });
        }
      } catch (e) { emit({ f: 'screen_tga_err', path: path, e: String(e) }); }
    }
    return 0;
  }, 'int', ['pointer']);
  Interceptor.replace(A(0x420f20), repl);
  emit({ f: 'hook', target: 'thumb_replace_0x420f20' });
} catch (e) { emit({ f: 'hookerr', name: 'thumb_replace', e: String(e) }); }

try {
  const gameNew = new NativeFunction(A(0x2fa7e6), 'pointer', ['uint']);
  const resampleShim = new NativeCallback(function (src, sw, sh, ch, dw, dh) {
    sw = sw | 0; sh = sh | 0; ch = ch | 0; dw = dw | 0; dh = dh | 0;
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
        (ch !== 3 && ch !== 4) || dw * dh * ch > 16 * 1024 * 1024) {
      emit({ f: 'resample_shim_reject', sw, sh, ch, dw, dh, tid: this.threadId });
      return ptr(0);
    }
    let out = null;
    try { out = gameNew(dw * dh * ch); } catch (e) {}
    if (!out || out.isNull()) out = Memory.alloc(dw * dh * ch);
    for (let y = 0; y < dh; y++) {
      const sy = Math.min(sh - 1, Math.floor(y * sh / dh));
      for (let x = 0; x < dw; x++) {
        const sx = Math.min(sw - 1, Math.floor(x * sw / dw));
        const s = (sy * sw + sx) * ch;
        const d = (y * dw + x) * ch;
        for (let c = 0; c < ch; c++) out.add(d + c).writeU8(src.add(s + c).readU8());
      }
    }
    emit({ f: 'resample_shim_0x45dad0', sw, sh, ch, dw, dh, tid: this.threadId });
    return out;
  }, 'pointer', ['pointer', 'int', 'int', 'int', 'int', 'int']);
  Interceptor.replace(A(0x45dad0), resampleShim);
  emit({ f: 'hook', target: 'resample_shim_0x45dad0' });
} catch (e) { emit({ f: 'hookerr', name: 'resample_shim', e: String(e) }); }

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

// ---- quicksave: action 0xDA in the input action dispatcher FUN_00621210 ----
// FUN_006227e0 runs the per-frame input update with ECX = the input object;
// action 0xDA is Quick Save (FUN_005df4e0/FUN_005edba0/FUN_005f4b50).
try {
  Interceptor.attach(A(0x4b58a0), {
    onEnter(args) {
      try {
        emit({ f: 'save_writer', self: this.context.ecx.toString(),
               a0: args[0].toString(), a1: args[1].toString(),
               a2: args[2].toString(), a3: args[3] ? args[3].toString() : null });
      } catch (e) {}
    }
  });
  emit({ f: 'hook', target: 'save_writer_0x4b58a0' });
} catch (e) { emit({ f: 'hookerr', name: 'save_writer', e: String(e) }); }
let inputObj = null;
let doQuicksave = false;
let actionFn = null;
try {
  Interceptor.attach(A(0x6227e0), {
    onEnter() { inputObj = this.context.ecx; }
  });
  actionFn = new NativeFunction(A(0x621210), 'void', ['pointer', 'int', 'int'], 'thiscall');
  const gl = Process.getModuleByName('opengl32.dll');
  Interceptor.attach(gl.getExportByName('wglSwapBuffers'), {
    onEnter() {
      if (!doQuicksave) return;
      doQuicksave = false;
      if (inputObj === null) { emit({ f: 'quicksave_error', e: 'no input object captured' }); return; }
      try {
        actionFn(inputObj, 0xda, 1);
        emit({ f: 'quicksave_called', fn: '0x621210', obj: inputObj.toString(), tid: this.threadId });
      } catch (e) { emit({ f: 'quicksave_error', e: String(e) }); }
    }
  });
  emit({ f: 'hook', target: 'quicksave_0x621210' });
} catch (e) { emit({ f: 'hookerr', name: 'quicksave', e: String(e) }); }

// ---- live NCS VM differential probe --------------------------------------
// ExecuteCode (0x5d2bd0): thiscall(vm, int* ip, byte* program, int length).
// VM fields: +0x1ac SP, +0x1b0 BP, +0x1b8 type array, +0x1bc value array.
// The action dispatcher (0x52c0d0) is called from inside ExecuteCode; capture
// the current IP/SP/BP and the action id/return for a trace replay.
let vmExecCount = 0;
let vmActionCount = 0;
const execByThread = new Map();
const seenPrograms = new Set();
const VM_HOOK_LIMIT = 400;
const ACTION_HOOK_LIMIT = 12000;
const PROGRAM_BYTES_LIMIT = 65536;

function hexOf(p, len) {
  try {
    const u8 = new Uint8Array(p.readByteArray(len));
    let s = '';
    for (let i = 0; i < u8.length; i++) s += u8[i].toString(16).padStart(2, '0');
    return s;
  } catch (e) { return null; }
}

function vmCells(vm, start, n) {
  const out = [];
  try {
    const types = vm.add(0x1b8).readPointer();
    const vals = vm.add(0x1bc).readPointer();
    const count = Math.max(0, Math.min(n, 1024));
    for (let i = 0; i < count; i++) {
      const idx = start + i;
      out.push({ t: types.add(idx).readU8(), v: vals.add(idx * 4).readS32() });
    }
  } catch (e) {}
  return out;
}

function vmTop(vm, sp, n) {
  const start = Math.max(0, sp - n);
  return vmCells(vm, start, Math.min(n, sp));
}

const probedStrings = new Set();
const deepProbedStrings = new Set();

// Live NWScript string cell: pointer to {char* data; int length; ...}.
function readVmString(v) {
  try {
    const p = ptr(v >>> 0);
    const data = p.readPointer();
    const len = p.add(4).readInt();
    if (len < 0 || len > 8192) return null;
    if (len === 0) return '';
    if (data.isNull()) return null;
    const raw = new Uint8Array(data.readByteArray(len));
    let s = '';
    for (let i = 0; i < raw.length; i++) {
      if (raw[i] === 0) break;
      s += String.fromCharCode(raw[i]);
    }
    return s;
  } catch (e) {
    return null;
  }
}

// Dump the memory behind a string cell once, so the CExoString layout can be
// recovered from a live trace.  All reads are guarded.
function probeStringCell(cell, force) {
  if (!cell || cell.t !== 5) return null;
  const v = cell.v >>> 0;
  if (force) {
    if (deepProbedStrings.has(v) || deepProbedStrings.size >= 1000) return null;
    deepProbedStrings.add(v);
  } else {
    if (probedStrings.has(v) || probedStrings.size >= 40) return null;
    probedStrings.add(v);
  }
  const p = ptr(v);
  const out = { v: '0x' + v.toString(16) };
  try { out.mem = hexOf(p, 32); } catch (e) { out.memerr = String(e); }
  try { out.len0 = p.readInt(); } catch (e) {}
  try { out.len4 = p.add(4).readInt(); } catch (e) {}
  try { out.cstr0 = p.readUtf8String(48); } catch (e) {}
  try { out.cstr4 = p.add(4).readUtf8String(48); } catch (e) {}
  try { out.cstr8 = p.add(8).readUtf8String(48); } catch (e) {}
  for (const off of [0, 4, 8, 12, 16]) {
    try {
      const q = p.add(off).readPointer();
      out['p' + off] = q.toString();
      try { out['s' + off] = q.readUtf8String(48); } catch (e) {}
    } catch (e) {}
  }
  return out;
}

function probeCells(cells, force) {
  const out = [];
  for (let i = 0; i < cells.length; i++) {
    const c = cells[i];
    const copy = { t: c.t, v: c.v };
    if (c.t === 5) {
      const s = readVmString(c.v);
      if (s !== null) copy.s = s;
      try {
        const d = ptr(c.v >>> 0).readUtf8String(64);
        if (d !== null && d.length) copy.direct = d;
      } catch (e) {}
      try { copy.raw16 = hexOf(ptr(c.v >>> 0), 16); } catch (e) {}
      try {
        const p = ptr(c.v >>> 0);
        const d = p.readPointer();
        const len = p.add(4).readInt();
        if (len >= 0 && len <= 8192) copy.datahex = hexOf(d, Math.min(len, 64));
      } catch (e) {}
    }
    const nearTop = i >= cells.length - 2;
    const probe = probeStringCell(c, force && nearTop);
    if (probe) copy.probe = probe;
    out.push(copy);
  }
  return out;
}

// Probe a returned cell as a string without ever faulting the host.  A
// NWScript string value is a pointer; try the common CExoString layouts.
function peekString(ptr) {
  const s0 = readVmString(ptr.toInt32());
  if (s0 !== null) return s0;
  try {
    const s = ptr.readUtf8String(64);
    if (s && s.length) return s;
  } catch (e) {}
  try {
    const s = ptr.readPointer().readUtf8String(64);
    if (s && s.length) return s;
  } catch (e) {}
  try {
    const s = ptr.add(4).readPointer().readUtf8String(64);
    if (s && s.length) return s;
  } catch (e) {}
  return null;
}

try {
  Interceptor.attach(A(0x5d2bd0), {
    onEnter(args) {
      try {
        const vm = this.context.ecx;
        const codePtr = args[0];
        const prog = args[1];
        const len = args[2].toInt32();
        const ip = codePtr.readS32();
        const sp = vm.add(0x1ac).readS32();
        const bp = vm.add(0x1b0).readS32();
        const tid = this.threadId;
        const ctx = { vm: vm, codePtr: codePtr, prog: prog, len: len, tid: tid };
        const stack = execByThread.get(tid) || [];
        stack.push(ctx);
        execByThread.set(tid, stack);
        if (vmExecCount < VM_HOOK_LIMIT) {
          vmExecCount++;
          let vtable = null;
          try { vtable = vm.readPointer().toString(); } catch (e) {}
          const o = { f: 'vm_exec', n: vmExecCount, vm: vm.toString(),
                      vtable: vtable, prog: prog.toString(), len: len,
                      ip: ip, sp: sp, bp: bp,
                      stack: vmTop(vm, sp, 8), tid: tid,
                      entry_cells: probeCells(vmCells(vm, 0, Math.min(sp, 1024))) };
          try {
            const key = prog.toString() + ':' + len;
            if (!seenPrograms.has(key) && len > 0 && len <= PROGRAM_BYTES_LIMIT) {
              seenPrograms.add(key);
              o.program_hex = hexOf(prog, len);
            } else {
              o.head = hexOf(prog, Math.min(len, 32));
            }
          } catch (e) { o.hexerr = String(e); }
          emit(o);
        }
      } catch (e) { emit({ f: 'vm_exec_error', e: String(e) }); }
    },
    onLeave(retval) {
      try {
        const tid = this.threadId;
        const stack = execByThread.get(tid) || [];
        const ctx = stack.pop();
        if (!ctx) return;
        const ip = ctx.codePtr.readS32();
        const sp = ctx.vm.add(0x1ac).readS32();
        const bp = ctx.vm.add(0x1b0).readS32();
        if (vmExecCount <= VM_HOOK_LIMIT) {
          emit({ f: 'vm_exec_exit', vm: ctx.vm.toString(), ret: retval.toInt32(),
                 ip: ip, sp: sp, bp: bp, tid: tid });
        }
      } catch (e) {}
    }
  });
  emit({ f: 'hook', target: 'execute_code_0x5d2bd0' });
} catch (e) { emit({ f: 'hookerr', name: 'execute_code', e: String(e) }); }

try {
  Interceptor.attach(A(0x52c0d0), {
    onEnter(args) {
      try {
        const tid = this.threadId;
        const stack = execByThread.get(tid) || [];
        const ctx = stack[stack.length - 1];
        this.vmctx = ctx || null;
        this.caller = this.returnAddress ? this.returnAddress.toString() : null;
        this.aid = args[0].toInt32();
        this.aarg = args[1].toInt32();
        this.ip = null;
        this.sp = null;
        this.bp = null;
        this.stack = [];
        if (ctx) {
          this.ip = ctx.codePtr.readS32();
          this.sp = ctx.vm.add(0x1ac).readS32();
          this.bp = ctx.vm.add(0x1b0).readS32();
          this.stack = probeCells(vmTop(ctx.vm, this.sp, 12));
        }
      } catch (e) {}
    },
    onLeave(retval) {
      try {
        if (!this.vmctx) {
          // Queued command execution path (RunCommand): the dispatcher is
          // called outside ExecuteCode, so there is no active script IP.
          if (vmActionCount < ACTION_HOOK_LIMIT) {
            vmActionCount++;
            emit({ f: 'vm_command', n: vmActionCount, id: this.aid,
                   arg: this.aarg, caller: this.caller,
                   ret: retval.toInt32(), tid: this.threadId });
          }
          return;
        }
        if (vmActionCount >= ACTION_HOOK_LIMIT) return;
        vmActionCount++;
        const sp2 = this.vmctx.vm.add(0x1ac).readS32();
        const ret = retval.toInt32();
        emit({ f: 'vm_action', n: vmActionCount, id: this.aid, arg: this.aarg,
               ret: ret, ret_str: peekString(retval), ip: this.ip,
               sp: this.sp, bp: this.bp, sp_after: sp2, stack: this.stack,
               ret_cells: probeCells(vmTop(this.vmctx.vm, sp2, 6), true),
               ret_stack: probeCells(vmTop(this.vmctx.vm, sp2, 6), true),
               tid: this.threadId });
      } catch (e) {}
    }
  });
  emit({ f: 'hook', target: 'action_diff_0x52c0d0' });
} catch (e) { emit({ f: 'hookerr', name: 'action_diff', e: String(e) }); }

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
  quicksave() {
    if (inputObj === null) return { ok: false, why: 'no input object captured' };
    doQuicksave = true;
    return { ok: true, queued: true, obj: inputObj.toString() };
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
