'use strict';
// Lane s1-06-dynamic (Sprint 1) in-process instrumentation.
// Build SHA-256 0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6,
// image base 0x400000. Read-only hooks plus two disclosed shims (screenshot ret at
// 0x420f20, gluBuild2DMipmaps -> GL_GENERATE_MIPMAP) required under Mesa llvmpipe;
// engine state is otherwise never modified. Hook addresses cited in
// docs/recon/08_VFS_DYNAMIC.md and docs/recon/12_COORDS_OPEN_ITEMS.md.

const CFG = {
  targets: ['end_m01aa', 'tar_m02af', 'module', 'm01aa', 'm02af', 'k_hcar_dialog', 'plc_bed1',
            'mainmenu', 'chargen', 'global', 'mainmenudx', 'chargendx'],
  addKeyRefs: ['end_m01aa', 'tar_m02af', 'module', 'm01aa', 'm02af'],
  addKeyTypes: [2011, 2012, 2014, 2023, 2057, 3002, 4],
  addKeyAll: false,
  addKeyLimit: 6000,
  dirNames: true,
  dirNameDirs: ['override', 'movies', 'streamwaves', 'streammusic'],
  dirNameLimit: 4096,
  fileTraceAll: false,
  traceBacktrace: false,
  cameraWatch: true,
};

const game = Process.getModuleByName('swkotor.exe');
const BASE = game.base;
const IMAGE_BASE = 0x400000;
function A(abs) { return BASE.add(abs - IMAGE_BASE); }
let seq = 0;
let modelN = 0;
let glGetStringCb = null;  // keep the NativeCallback alive for the IAT patch
function emit(o) { o.seq = ++seq; o.t = Date.now(); send(o); }
function cstra(p, max) { try { return p.readAnsiString(max || 256); } catch (e) { return null; } }
function cstrw(p) { try { return p.readUtf16String(1024); } catch (e) { return null; } }
function u32(p, off) { try { return p.add(off).readU32() >>> 0; } catch (e) { return null; } }
function u16(p, off) { try { return p.add(off).readU16() >>> 0; } catch (e) { return null; } }
function ptrAt(p, off) { try { return p.add(off).readPointer(); } catch (e) { return null; } }
function hs(p) { return p ? p.toString() : null; }
function bt(ctx, n) {
  if (!CFG.traceBacktrace) return [];
  try { return Thread.backtrace(ctx, Backtracer.ACCURATE).slice(0, n || 10).map((a) => hs(a)); }
  catch (e) { return ['bt_error']; }
}
function loaderName(obj) {
  const q = ptrAt(obj, 0x20);
  if (!q || q.isNull()) return null;
  const s = cstra(q, 128);
  if (!s) return null;
  const z = s.indexOf('\x00');
  return z >= 0 ? s.slice(0, z) : s;
}
function slotResref(p) {
  try {
    const b = new Uint8Array(p.readByteArray(16));
    let s = '';
    for (let i = 0; i < b.length && b[i] !== 0; i++) s += String.fromCharCode(b[i]);
    return s.toLowerCase();
  } catch (e) { return null; }
}
function wantResref(rr) { return rr !== null && CFG.targets.indexOf(rr) >= 0; }

// ---- Mesa GL shim: glGetString(0x8874) must never return NULL -------------
// The engine dereferences the result at 0x47a2ce without a null check; Mesa
// returns NULL for GL_PROGRAM_ERROR_STRING_NV (0x8874).  Intercept the
// opengl32 export (the engine's IAT points at it) and replace just that
// enum's return with an empty string; all other enums pass through.
try {
  const gl = Process.getModuleByName('opengl32.dll');
  const realGetString = new NativeFunction(gl.getExportByName('glGetString'),
                                           'pointer', ['uint'], 'stdcall');
  const emptyString = Memory.allocUtf8String('');
  glGetStringCb = new NativeCallback(function (which) {
    if ((which >>> 0) === 0x8874) {
      emit({ f: 'glGetString_shim', which: '0x8874', tid: this.threadId });
      return emptyString;
    }
    try { return realGetString(which); } catch (e) { return ptr(0); }
  }, 'pointer', ['uint'], 'stdcall');
  const cbPtr = ptr(glGetStringCb.toString());
  const iat = BASE.add(0x33d290);  // IAT RVA, not an absolute VA
  const page = ptr(iat.toUInt32() & 0xfffff000);
  const range = Process.findRangeByAddress(iat);
  const prot = range ? range.protection : 'r--';
  Memory.protect(page, 0x1000, 'rw-');
  const old = iat.readPointer();
  iat.writePointer(cbPtr);
  Memory.protect(page, 0x1000, prot);
  emit({ f: 'iat_patch', tag: 'glGetString_0x8874', at: hs(iat),
         old: hs(old), newv: cbPtr.toString(), prot: prot });
} catch (e) {
  emit({ f: 'hookerr', name: 'glGetString_shim', e: String(e) });
}

// ---- archive loaders ------------------------------------------------------
function hookLoader(off, name) {
  try {
    Interceptor.attach(A(off), {
      onEnter() {
        this.obj = this.context.ecx;
        emit({ f: name, phase: 'enter', obj: this.obj.toString(), name: loaderName(this.obj),
               idx: u32(this.obj, 0x28), count: u32(this.obj, 0x14), tid: this.threadId });
      },
      onLeave(ret) {
        emit({ f: name, phase: 'leave', obj: this.obj.toString(), name: loaderName(this.obj),
               idx: u32(this.obj, 0x28), count: u32(this.obj, 0x14), ret: ret.toString(),
               tid: this.threadId });
      }
    });
    emit({ f: 'hook', target: name, addr: '0x' + off.toString(16) });
  } catch (e) { emit({ f: 'hookerr', name: name, e: String(e) }); }
}
hookLoader(0x40f3c0, 'loader_erf');
hookLoader(0x40f990, 'loader_rim');
hookLoader(0x40fb80, 'loader_key');

// ---- CExoKeyTable::AddKey -------------------------------------------------
let addKeyN = 0;
try {
  Interceptor.attach(A(0x40e990), {
    onEnter(args) {
      this.obj = this.context.ecx;
      let rr = cstra(args[0], 40);
      if (rr) { const z = rr.indexOf('\x00'); if (z >= 0) rr = rr.slice(0, z); }
      this.rr = rr ? rr.toLowerCase() : null;
      this.packed = args[1].toInt32() >>> 0;
      this.type = this.packed & 0xffff;
      this.tname = loaderName(this.obj);
      this.want = addKeyN < CFG.addKeyLimit &&
        (CFG.addKeyAll || (this.rr && (CFG.addKeyRefs.indexOf(this.rr) >= 0 ||
          CFG.targets.indexOf(this.rr) >= 0)) || CFG.addKeyTypes.indexOf(this.type) >= 0);
      if (this.want) {
        addKeyN++;
        emit({ f: 'addkey', phase: 'enter', table: this.obj.toString(), resref: this.rr,
               type: this.type, name: this.tname, tid: this.threadId });
      }
    },
    onLeave(ret) {
      if (!this.want) return;
      emit({ f: 'addkey', phase: 'leave', table: this.obj.toString(), resref: this.rr,
             type: this.type, name: this.tname, ret: ret.toInt32(), tid: this.threadId });
    }
  });
  emit({ f: 'hook', target: 'addkey', addr: '0x40e990' });
} catch (e) { emit({ f: 'hookerr', name: 'addkey', e: String(e) }); }

// ---- per-pass lookup + full resman lookup ---------------------------------
try {
  Interceptor.attach(A(0x4071a0), {
    onEnter(args) {
      let rr = cstra(args[1], 40);
      if (rr) { const z = rr.indexOf('\x00'); if (z >= 0) rr = rr.slice(0, z); }
      this.rr = rr ? rr.toLowerCase() : null;
      this.type = args[2].toInt32();
      this.filter = args[5].toInt32();
      this.outTable = args[3];
      this.outEntry = args[4];
      this.want = wantResref(this.rr);
      this.tid = this.threadId;
    },
    onLeave(ret) {
      if (!this.want) return;
      const t = ret.toInt32() !== 0 && this.outTable && !this.outTable.isNull() ? ptrAt(this.outTable, 0) : null;
      const e = ret.toInt32() !== 0 && this.outEntry && !this.outEntry.isNull() ? ptrAt(this.outEntry, 0) : null;
      emit({ f: 'pass', resref: this.rr, type: this.type, filter: this.filter,
             hit: ret.toInt32(), table: t ? t.toString() : null, tableName: t ? loaderName(t) : null,
             slotResref: e ? slotResref(e) : null, slotType: e ? u16(e, 0x1a) : null, tid: this.tid });
    }
  });
  emit({ f: 'hook', target: 'pass', addr: '0x4071a0' });
} catch (e) { emit({ f: 'hookerr', name: 'pass', e: String(e) }); }

try {
  Interceptor.attach(A(0x407230), {
    onEnter(args) {
      let rr = cstra(args[0], 40);
      if (rr) { const z = rr.indexOf('\x00'); if (z >= 0) rr = rr.slice(0, z); }
      this.rr = rr ? rr.toLowerCase() : null;
      this.type = args[1].toInt32();
      this.outTable = args[2];
      this.outEntry = args[3];
      this.want = wantResref(this.rr) || this.type === 4 || this.type === 2029 ||
        ((this.type === 2002 || this.type === 3008) && modelN < 900);
      if ((this.type === 2002 || this.type === 3008)) modelN++;
      this.tid = this.threadId;
    },
    onLeave(ret) {
      if (!this.want) return;
      const ok = ret.toInt32() !== 0;
      const t = ok && this.outTable && !this.outTable.isNull() ? ptrAt(this.outTable, 0) : null;
      const e = ok && this.outEntry && !this.outEntry.isNull() ? ptrAt(this.outEntry, 0) : null;
      emit({ f: 'resman', resref: this.rr, type: this.type, ret: ret.toInt32(),
             table: t ? t.toString() : null, tableName: t ? loaderName(t) : null,
             slotResref: e ? slotResref(e) : null, slotType: e ? u16(e, 0x1a) : null, tid: this.tid });
    }
  });
  emit({ f: 'hook', target: 'resman', addr: '0x407230' });
} catch (e) { emit({ f: 'hookerr', name: 'resman', e: String(e) }); }

// ---- file opens / directory enumeration ------------------------------------
const k32 = Process.getModuleByName('kernel32.dll');
const KEEP_EXTS = ['.erf','.rim','.mod','.sav','.nwm','.2da','.tpc','.tga','.ifo','.are','.git','.key','.bif','.wav','.bik','.mp3','.wma','.rsv','.ini','.mdl','.mdx','.pwk','.bwm','.lip','.ssf','.dlg','.ncs','.nss','.utc','.utt','.utp','.utd','.utm','.jrl','.bic']
const KEEP_DIRS = ['saves','currentgame','gameinprogress','futuregame','override'];
function tracePath(path) {
  if (!path) return false;
  if (CFG.fileTraceAll) return true;
  const low = path.toLowerCase();
  const dot = low.lastIndexOf('.');
  if (dot >= 0 && KEEP_EXTS.indexOf(low.slice(dot)) >= 0) return true;
  return KEEP_DIRS.some((d) => low.indexOf(String.fromCharCode(92) + d + String.fromCharCode(92)) >= 0);
}
function hookFile(name, wide) {
  try {
    Interceptor.attach(k32.findExportByName(name), {
      onEnter(args) {
        this.path = wide ? cstrw(args[0]) : cstra(args[0]);
        this.access = args[1].toInt32();
        this.disp = args[4].toInt32();
        this.want = tracePath(this.path);
      },
      onLeave(ret) {
        if (!this.want) return;
        const ok = !ret.isNull() && ret.toInt32() !== -1;
        if (ok) { try { handlePath.set(ret.toString(), this.path); } catch (e) {} }
        emit({ f: 'file', api: name, path: this.path, access: this.access, disp: this.disp,
              
               ok: ok, tid: this.threadId });
      }
    });
  } catch (e) { emit({ f: 'hookerr', name: name, e: String(e) }); }
}
hookFile('CreateFileW', true);
hookFile('CreateFileA', false);

function dirKeepTest(path) { const low = path.toLowerCase(); const bs = String.fromCharCode(92); return ['streammusic','streamwaves','movies','override'].some((d) => low.indexOf(bs + d) >= 0 || low.indexOf(d + bs) >= 0); }
const findHandles = new Map();
function dirEnabled(path) {
  if (!CFG.dirNames || !path) return false;
  const low = path.toLowerCase();
  const bs = String.fromCharCode(92); return CFG.dirNameDirs.some((d) => low.indexOf(bs + d) >= 0 || low.indexOf(d + bs) >= 0);
}
function hookFind(name, wide) {
  try {
    Interceptor.attach(k32.findExportByName(name), {
      onEnter(args) {
        this.path = wide ? cstrw(args[0]) : cstra(args[0]);
        this.emitAny = !!(this.path && dirKeepTest(this.path));
      },
      onLeave(ret) {
        if (this.emitAny) emit({ f: 'dir', api: name, path: this.path, ret: ret.toString(), tid: this.threadId });
        if (dirEnabled(this.path)) findHandles.set(ret.toString(), { path: this.path, names: [], n: 0 });
      }
    });
  } catch (e) { emit({ f: 'hookerr', name: name, e: String(e) }); }
}
hookFind('FindFirstFileW', true);
hookFind('FindFirstFileA', false);
hookFind('FindFirstFileExW', true);
function hookFindNext(name, wide) {
  try {
    Interceptor.attach(k32.findExportByName(name), {
      onEnter(args) {
        this.h = args[0].toString();
        this.data = args[1];
        this.rec = findHandles.get(this.h) || null;
      },
      onLeave(ret) {
        if (!this.rec) return;
        const ok = ret.toInt32() !== 0;
        let nm = null;
        try { nm = wide ? this.data.add(0x2c).readUtf16String(260) : this.data.add(0x2c).readAnsiString(260); }
        catch (e) { nm = null; }
        this.rec.n++;
        if (ok && nm !== null && this.rec.names.length < CFG.dirNameLimit) {
          this.rec.names.push(nm);
          if (/\.wav$/i.test(nm)) {
            emit({ f: 'dirnext_wav', path: this.rec.path, i: this.rec.n, name: nm, tid: this.threadId });
          }
        }
      }
    });
  } catch (e) { emit({ f: 'hookerr', name: name, e: String(e) }); }
}
hookFindNext('FindNextFileW', true);
hookFindNext('FindNextFileA', false);
try {
  Interceptor.attach(k32.findExportByName('FindClose'), {
    onEnter(args) { this.h = args[0].toString(); this.rec = findHandles.get(this.h) || null; },
    onLeave() {
      if (!this.rec) return;
      emit({ f: 'dir_close', path: this.rec.path, count: this.rec.n,
             wavs: this.rec.names.filter((n) => /\.wav$/i.test(n)).length,
             first: this.rec.names.slice(0, 12), last: this.rec.names.slice(-6), tid: this.threadId });
      findHandles.delete(this.h);
    }
  });
} catch (e) { emit({ f: 'hookerr', name: 'FindClose', e: String(e) }); }

// ---- dialogue camera fields (docs/recon/12; VAs verified against the build) ----
// 0x6bb670 dialogue-camera ctor; write site 0x6bb6f8: mov [esi+0x38], ecx
// 0x62fed0 dialogue-controller ctor; write site 0x630244: mov [esi+0xc4], ebx
// camera-creation reads 0x62bf4e/0x6320c9/0x632127: mov ecx, [esi+0xc4]; mov ecx, eax; call 0x6412f0
function camHook(off, name, fn) {
  try {
    Interceptor.attach(A(off), {
      onEnter(args) {
        if (!CFG.cameraWatch) return;
        try { fn.call(this, args); } catch (e) { emit({ f: 'hookerr', name: name, e: String(e) }); }
      }
    });
    emit({ f: 'hook', target: name, addr: '0x' + off.toString(16) });
  } catch (e) { emit({ f: 'hookerr', name: name, e: String(e) }); }
}
try {
  Interceptor.attach(A(0x6bb670), {
    onEnter() { this.cam = this.context.ecx; },
    onLeave(ret) {
      if (!CFG.cameraWatch) return;
      emit({ f: 'camera_ctor', phase: 'leave', cam: this.cam.toString(),
             mode38: u32(this.cam, 0x38), closeup34: u32(this.cam, 0x34),
             vtable: hs(ptrAt(this.cam, 0)), ret: ret.toString(), tid: this.threadId });
    }
  });
  emit({ f: 'hook', target: 'camera_ctor', addr: '0x6bb670' });
} catch (e) { emit({ f: 'hookerr', name: 'camera_ctor', e: String(e) }); }
camHook(0x6bb6f8, 'camera_mode_write', function () {
  emit({ f: 'camera_mode_write', cam: this.context.esi.toString(),
         value: this.context.ecx.toInt32() >>> 0, tid: this.threadId });
});
try {
  Interceptor.attach(A(0x62fed0), {
    onEnter() { this.ctrl = this.context.ecx; },
    onLeave(ret) {
      if (!CFG.cameraWatch) return;
      emit({ f: 'controller_ctor', phase: 'leave', ctrl: this.ctrl.toString(),
             c4: u32(this.ctrl, 0xc4), f38: u32(this.ctrl, 0x38), f34: u32(this.ctrl, 0x34),
             ret: ret.toString(), tid: this.threadId });
    }
  });
  emit({ f: 'hook', target: 'controller_ctor', addr: '0x62fed0' });
} catch (e) { emit({ f: 'hookerr', name: 'controller_ctor', e: String(e) }); }
camHook(0x630244, 'controller_mode_write', function () {
  emit({ f: 'controller_mode_write', ctrl: this.context.esi.toString(),
         value: this.context.ebx.toInt32() >>> 0, tid: this.threadId });
});
const camReadSites = [0x62bf4e, 0x6320c9, 0x632127];
for (const off of camReadSites) {
  camHook(off, 'camera_mode_read_0x' + off.toString(16), function () {
    emit({ f: 'camera_mode_read', site: '0x' + off.toString(16),
           controller: this.context.esi.toString(), c4: u32(this.context.esi, 0xc4),
           camera: this.context.eax.toString(), tid: this.threadId });
  });
}

// ---- disclosed shims: save-thumbnail ret + glu mipmap (Mesa llvmpipe) ------
// ret-patch of 0x420f20 is applied in the synth block below (attach first)

try {
  Interceptor.attach(A(0x45dad0), {
    onEnter(a) {
      emit({ f: 'resample_0x45dad0', sw: a[1].toInt32(), sh: a[2].toInt32(),
             dw: a[4].toInt32(), dh: a[5].toInt32(), tid: this.threadId });
    }
  });
} catch (e) { emit({ f: 'hookerr', name: 'resample', e: String(e) }); }
try {
  const g = Process.getModuleByName('opengl32.dll');
  const glu = Process.getModuleByName('GLU32.dll');
  if (g && glu) {
    const texParameteri = g.getExportByName('glTexParameteri');
    const build = glu.getExportByName('gluBuild2DMipmaps');
    const pTexParam = new NativeFunction(texParameteri, 'void', ['uint', 'uint', 'int'], 'stdcall');
    const repl = new NativeCallback(function (target, internalformat, width, height, format, type, data) {
      try { pTexParam(target, 0x8191, 1); } catch (e) {}
      return 0;
    }, 'int', ['uint', 'int', 'int', 'int', 'uint', 'uint', 'pointer'], 'stdcall');
    Interceptor.replace(build, repl);
    emit({ f: 'hook', target: 'gluBuild2DMipmaps_shim', addr: build.toString() });
  } else {
    emit({ f: 'glu_shim_skipped', reason: 'no opengl32/GLU32 module' });
  }
} catch (e) { emit({ f: 'hookerr', name: 'glu_shim', e: String(e) }); }

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
  setconfig(c) {
    if (!c) return { ok: false };
    if (Array.isArray(c.targets)) CFG.targets = c.targets.map((x) => String(x).toLowerCase());
    for (const k of ['addKeyAll', 'dirNames', 'fileTraceAll', 'traceBacktrace', 'cameraWatch']) {
      if (typeof c[k] === 'boolean') CFG[k] = c[k];
    }
    if (Array.isArray(c.addKeyRefs)) CFG.addKeyRefs = c.addKeyRefs.map((x) => String(x).toLowerCase());
    if (Array.isArray(c.addKeyTypes)) CFG.addKeyTypes = c.addKeyTypes.map((x) => Number(x));
    return { ok: true, cfg: CFG };
  },
  stats() { return { seq: seq, addKeyN: addKeyN, meshTotal: (typeof meshTotal !== 'undefined' ? meshTotal : -1) }; },
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
  }
};
emit({ f: 'agent_s1_06', base: BASE.toString(), pid: Process.id, cfg: CFG });

// ---- Sprint-1 closure: N-group mesh processing (docs/recon/23 #11-#13) -------
// build sha256 0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6
// Mesh data header: +0x100 face-count array ptr (+0x104/+0x108 counts),
// +0x10c handles ptr (+0x110/+0x114), +0x118 group-offsets ptr, +0x11c/+0x120 group count.
function meshFields(r) {
  return { f100: u32(r, 0x100), f104: u32(r, 0x104), f108: u32(r, 0x108),
           f10c: u32(r, 0x10c), f110: u32(r, 0x110), f114: u32(r, 0x114),
           f118: u32(r, 0x118), f11c: u32(r, 0x11c), f120: u32(r, 0x120) };
}
function isMultiGroup(f) {
  return f && (f.f104 > 1 || f.f11c > 1 || f.f120 > 1 || f.f108 > 1 || f.f110 > 1 || f.f114 > 1);
}
let meshLogN = 0;
function emitMesh(o) {
  if (meshLogN < 400) { meshLogN++; emit(o); }
}
// relocation FUN_004a0c00: guarded adds + per-group loop 0x4a0ce0-0x4a0d7b
let meshTotal = 0;
camHook(0x4a0c5e, 'reloc_add_faces_0x4a0c5e', function () {
  const f = meshFields(this.context.ebx);
  meshTotal++;
  if (isMultiGroup(f)) {
    emitMesh({ f: 'reloc_faces', site: '0x4a0c5e', hdr: this.context.ebx.toString(), fields: f, multi: true, tid: this.threadId });
  }
});
camHook(0x4a0ce0, 'reloc_group_loop_0x4a0ce0', function () {
  if (u32(this.context.ebx, 0x11c) <= 1) return;
  const f = meshFields(this.context.ebx);
  if (isMultiGroup(f)) {
    emitMesh({ f: 'reloc_group', site: '0x4a0ce0', hdr: this.context.ebx.toString(),
               g: this.context.eax.toInt32(), fields: f, tid: this.threadId });
  }
});
camHook(0x4a0d68, 'reloc_group_tail_0x4a0d68', function () {
  if (u32(this.context.ebx, 0x11c) <= 1) return;
  const f = meshFields(this.context.ebx);
  if (isMultiGroup(f)) {
    emitMesh({ f: 'reloc_group_tail', site: '0x4a0d68', hdr: this.context.ebx.toString(),
               eax: this.context.eax.toInt32(), ecx: this.context.ecx.toInt32(),
               groupCount: f.f11c, tid: this.threadId });
  }
});
// GPU draw FUN_0046f900: bound [esi+0x104] at 0x46f967, loop head 0x46f990
camHook(0x46f967, 'draw_bound_0x46f967', function () {
  if (u32(this.context.esi, 0x104) <= 1 && u32(this.context.esi, 0x11c) <= 1) return;
  const f = meshFields(this.context.esi);
  if (isMultiGroup(f)) {
    emitMesh({ f: 'draw_bound', site: '0x46f967', mesh: this.context.esi.toString(), fields: f, tid: this.threadId });
  }
});
camHook(0x46f990, 'draw_group_loop_0x46f990', function () {
  if (u32(this.context.esi, 0x104) <= 1 && u32(this.context.esi, 0x11c) <= 1) return;
  const f = meshFields(this.context.esi);
  if (isMultiGroup(f)) {
    emitMesh({ f: 'draw_group', site: '0x46f990', mesh: this.context.esi.toString(),
               g: this.context.edi.toInt32(), ebp: this.context.ebp.toInt32(),
               offsetsArr: hs(ptrAt(this.context.esi, 0x118)),
               countsArr: hs(ptrAt(this.context.esi, 0x100)),
               offsets_g: ptrAt(ptrAt(this.context.esi, 0x118), this.context.edi.toInt32() * 4) ? u32(ptrAt(this.context.esi, 0x118), this.context.edi.toInt32() * 4) : null,
               counts_g: u32(ptrAt(this.context.esi, 0x100), this.context.edi.toInt32() * 4),
               tid: this.threadId });
  }
});
camHook(0x477a80, 'draw2_group_loop_0x477a80', function () {
  if (u32(this.context.edi, 0x104) <= 1 && u32(this.context.edi, 0x11c) <= 1) return;
  const f = meshFields(this.context.edi);
  if (isMultiGroup(f)) {
    emitMesh({ f: 'draw2_group', site: '0x477a80', mesh: this.context.edi.toString(),
               g: this.context.ebx.toInt32(), bound: this.context.eax.toInt32(), fields: f, tid: this.threadId });
  }
});
camHook(0x487760, 'cpuskin_group_loop_0x487760', function () {
  if (u32(this.context.eax, 0x11c) <= 1) return;
  const f = meshFields(this.context.eax);
  if (isMultiGroup(f)) {
    emitMesh({ f: 'cpuskin_group', site: '0x487760', mesh: this.context.eax.toString(),
               g: this.context.ebp.toInt32(), ecx: this.context.ecx.toInt32(), fields: f, tid: this.threadId });
  }
});

function pickName(args) {
  for (let i = 0; i < 3; i++) {
    for (const a of [args[i], ptrAt(args[i], 0)]) {
      try {
        const s = cstra(a, 64);
        if (s && /^[a-zA-Z0-9_]{2,}$/.test(s)) return s;
      } catch (e) {}
      try {
        const w = cstrw(a, 32);
        if (w && /^[a-zA-Z0-9_]{2,}$/.test(w)) return w;
      } catch (e) {}
    }
  }
  return null;
}

// ---- Sprint-1 closure: supermodel clip chain (docs/recon/23 #14-#16) ---------
// FUN_004822a0 clip lookup: local array +0x58/+0x5c, super link +0x64.
let clipN = 0;
const clipDepth = {};
try {
  Interceptor.attach(A(0x4822a0), {
    onEnter(args) {
      this.obj = this.context.ecx;
      this.nm = pickName(args);
      const nm = this.nm;
      const tid = this.threadId;
      const d = (clipDepth[tid] || 0) + 1;
      clipDepth[tid] = d;
      this.d = d;
      if (clipN < 1200) {
        clipN++;
        emit({ f: 'clip_lookup', phase: d > 1 ? 'super_enter' : 'enter', depth: d,
               obj: this.obj.toString(), name: nm,
               local: hs(ptrAt(this.obj, 0x58)), localCount: u32(this.obj, 0x5c),
               super: hs(ptrAt(this.obj, 0x64)), tid: tid });
      }
    },
    onLeave(ret) {
      clipDepth[this.threadId] = Math.max(0, (clipDepth[this.threadId] || 1) - 1);
      if (clipN <= 1200) {
        emit({ f: 'clip_lookup', phase: 'leave', depth: this.d, obj: this.obj.toString(),
               name: this.nm, ret: ret.toInt32(), hit: ret.toInt32() !== 0, tid: this.threadId });
      }
    }
  });
  emit({ f: 'hook', target: 'clip_lookup_0x4822a0', addr: '0x4822a0' });
} catch (e) { emit({ f: 'hookerr', name: 'clip_lookup', e: String(e) }); }
camHook(0x482463, 'clip_super_link_0x482463', function () {
  const o = this.context.esi;
  emit({ f: 'clip_super_link', site: '0x482463', obj: o.toString(),
         super: hs(ptrAt(o, 0x64)), local: hs(ptrAt(o, 0x58)), localCount: u32(o, 0x5c),
         tid: this.threadId });
});
camHook(0x482459, 'clip_local_miss_0x482459', function () {
  emit({ f: 'clip_local_miss', site: '0x482459', obj: this.context.esi.toString(),
         super: hs(ptrAt(this.context.esi, 0x64)), tid: this.threadId });
});
let playN = 0;
try {
  Interceptor.attach(A(0x485bd0), {
    onEnter(args) {
      this.obj = this.context.esi;
      this.nm = pickName(args);
      const nm = this.nm;
      if (playN < 300) {
        playN++;
        emit({ f: 'clip_play', phase: 'enter', obj: this.obj.toString(), name: nm,
               active: hs(ptrAt(this.obj, 0xb0)), activeCount: u32(this.obj, 0xb4),
               local: hs(ptrAt(this.obj, 0x58)), localCount: u32(this.obj, 0x5c),
               super: hs(ptrAt(this.obj, 0x64)), tid: this.threadId });
      }
    },
    onLeave(ret) {
      if (playN <= 300) {
        emit({ f: 'clip_play', phase: 'leave', obj: this.obj.toString(), name: this.nm,
               ret: ret.toInt32(), tid: this.threadId });
      }
    }
  });
  emit({ f: 'hook', target: 'clip_play_0x485bd0', addr: '0x485bd0' });
} catch (e) { emit({ f: 'hookerr', name: 'clip_play', e: String(e) }); }
let queryN = 0;
try {
  Interceptor.attach(A(0x484d20), {
    onEnter(args) {
      this.obj = this.context.ecx;
      this.nm = pickName(args);
      const nm = this.nm;
      if (queryN < 200) {
        queryN++;
        emit({ f: 'clip_query', phase: 'enter', obj: this.obj.toString(), name: this.nm,
               active: hs(ptrAt(this.obj, 0xb0)), activeCount: u32(this.obj, 0xb4),
               super: hs(ptrAt(this.obj, 0x64)), tid: this.threadId });
      }
    },
    onLeave(ret) {
      if (queryN <= 200) {
        emit({ f: 'clip_query', phase: 'leave', obj: this.obj.toString(), name: this.nm,
               ret: ret.toInt32(), tid: this.threadId });
      }
    }
  });
  emit({ f: 'hook', target: 'clip_query_0x484d20', addr: '0x484d20' });
} catch (e) { emit({ f: 'hookerr', name: 'clip_query', e: String(e) }); }

// ---- Sprint-1 closure: replace the save-thumbnail writer 0x420f20 (cdecl, 1 arg) --
// so the Mesa-faulting resampler FUN_0045dad0 (faults at 0x45db21) never runs; the
// replacement writes a synthesized 256x256 24bpp TGA to the destination path instead.
try {
  const k32w = Process.getModuleByName('kernel32.dll');
  const createFileW = new NativeFunction(k32w.getExportByName('CreateFileW'), 'pointer',
    ['pointer', 'uint', 'uint', 'pointer', 'uint', 'uint', 'pointer'], 'stdcall');
  const writeFile = new NativeFunction(k32w.getExportByName('WriteFile'), 'int',
    ['pointer', 'pointer', 'uint', 'pointer', 'pointer'], 'stdcall');
  const closeHandle = new NativeFunction(k32w.getExportByName('CloseHandle'), 'int', ['pointer'], 'stdcall');
  const tga = new Uint8Array(18 + 256 * 256 * 3);
  tga[2] = 2; tga[12] = 0; tga[13] = 1; tga[14] = 0; tga[15] = 1; tga[16] = 24;
  for (let i = 18; i < tga.length; i += 3) { tga[i] = 0x60; tga[i + 1] = 0x60; tga[i + 2] = 0x60; }
  function synthPath(arg) {
    for (const a of [arg, ptrAt(arg, 0)]) {
      try { const w = cstrw(a, 260); if (w && /screen\.tga$/i.test(w)) return w; } catch (e) {}
      try { const c = cstra(a, 260); if (c && /screen\.tga$/i.test(c)) return c; } catch (e) {}
    }
    return null;
  }
  const repl = new NativeCallback(function () {
    // 0-arg default-abi replacement == plain `ret` for this cdecl target (caller cleans).
    // The single stack arg (dest path) sits at [esp+4] on entry.
    const arg = Memory.readPointer(this.context.esp.add(4));
    const path = synthPath(arg);
    emit({ f: 'thumb_replace_0x420f20', arg: hs(arg), path: path, tid: this.threadId });
    if (path) {
      try {
        const p = Memory.allocUtf16String(path);
        const h = createFileW(p, 0x40000000, 0, null, 2, 0, null);
        if (h && !h.isNull() && h.toInt32() !== -1) {
          const buf = Memory.alloc(tga.length);
          buf.writeByteArray(tga);
          const written = Memory.alloc(4);
          writeFile(h, buf, tga.length, written, null);
          closeHandle(h);
          emit({ f: 'screen_tga_synth', path: path, bytes: tga.length, written: written.readU32(), tid: this.threadId });
        }
      } catch (e) { emit({ f: 'screen_tga_err', path: path, e: String(e) }); }
    }
    return 0;
  }, 'int', []);
  Interceptor.replace(A(0x420f20), repl);
  emit({ f: 'hook', target: 'thumb_replace_0x420f20', addr: '0x420f20' });
} catch (e) { emit({ f: 'hookerr', name: 'thumb_replace', e: String(e) }); }
// Clean-room replacement for the faulting resampler FUN_0045dad0:
// (src, src_w, src_h, channels, dst_w, dst_h) -> newly allocated dst pixels.
// Nearest-neighbour is deterministic and avoids the Mesa-faulting box path.
// This is the JS twin of tools/harness/thumb_shim.py resample_nearest().
try {
  const resampleShim = new NativeCallback(function (src, sw, sh, ch, dw, dh) {
    sw = sw | 0; sh = sh | 0; ch = ch | 0; dw = dw | 0; dh = dh | 0;
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
        (ch !== 3 && ch !== 4) || dw * dh * ch > 16 * 1024 * 1024) {
      emit({ f: 'resample_shim_reject', sw, sh, ch, dw, dh, tid: this.threadId });
      return ptr(0);
    }
    const out = Memory.alloc(dw * dh * ch);
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
  emit({ f: 'hook', target: 'resample_shim_0x45dad0', addr: '0x45dad0' });
} catch (e) {
  emit({ f: 'hookerr', name: 'resample_shim', e: String(e) });
  try {
    Interceptor.attach(A(0x45dad0), {
      onEnter(a) {
        emit({ f: 'resample_0x45dad0_ENTERED', sw: a[1].toInt32(), sh: a[2].toInt32(),
               dw: a[4].toInt32(), dh: a[5].toInt32(), tid: this.threadId });
      }
    });
  } catch (e2) { emit({ f: 'hookerr', name: 'resample', e: String(e2) }); }
}

emit({ f: 'agent_s1_06_closure', pid: Process.id });
