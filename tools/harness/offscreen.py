#!/usr/bin/env python3
"""Run harness game processes windowed, off-screen, and at idle priority.

Windows has no built-in Xvfb. This helper provides the two practical
equivalents for a user-owned local process:

  launch  start the executable on a private Win32 desktop with
          SW_SHOWMINNOACTIVE and IDLE_PRIORITY_CLASS. The window never appears
          on the interactive desktop, so it cannot take focus or cover the
          user's screen.
  park    move all visible windows of an already-running process to
          (-32000, -32000), minimize them without activation, and drop the
          process to idle priority.
  watch   park every matching process at a fixed interval (for harnesses that
          spawn the game themselves, for example through Frida).

PrintWindow with PW_RENDERFULLCONTENT still works for many windowed D3D
applications while parked; if it does not, capture through the renderer or
in-process instead of taking a desktop screenshot.

Examples:
  python tools/harness/offscreen.py launch --exe <game.exe> --cwd <profile> --wait
  python tools/harness/offscreen.py park --pid 1234
  python tools/harness/offscreen.py watch --name swkotor.exe --minutes 60
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

SW_SHOWNOACTIVATE = 4
SW_MINIMIZE = 6
SWP_NOSIZE = 0x0001
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
IDLE_PRIORITY_CLASS = 0x00000040
PROCESS_SET_INFORMATION = 0x0200
CREATE_SUSPENDED = 0x00000004
CREATE_NEW_PROCESS_GROUP = 0x00000200
CREATE_UNICODE_ENVIRONMENT = 0x00000400
STARTF_USESHOWWINDOW = 0x00000001
GENERIC_ALL = 0x10000000
TH32CS_SNAPPROCESS = 0x00000002

user32.EnumWindows.argtypes = [ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM), wt.LPARAM]
user32.EnumWindows.restype = wt.BOOL
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowThreadProcessId.restype = wt.DWORD
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.IsWindowVisible.restype = wt.BOOL
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.ShowWindow.restype = wt.BOOL
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, ctypes.c_uint]
user32.SetWindowPos.restype = wt.BOOL
user32.CreateDesktopW.argtypes = [wt.LPCWSTR, wt.LPCWSTR, ctypes.c_void_p,
                                  wt.DWORD, wt.DWORD, ctypes.c_void_p]
user32.CreateDesktopW.restype = wt.HANDLE
user32.CloseDesktop.argtypes = [wt.HANDLE]
user32.CloseDesktop.restype = wt.BOOL

kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.SetPriorityClass.argtypes = [wt.HANDLE, wt.DWORD]
kernel32.SetPriorityClass.restype = wt.BOOL

EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)), ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD), ("szExeFile", wt.WCHAR * 260),
    ]


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR),
        ("lpTitle", wt.LPWSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD),
        ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD), ("dwXCountChars", wt.DWORD),
        ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD),
        ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
        ("lpReserved2", ctypes.POINTER(ctypes.c_byte)), ("hStdInput", wt.HANDLE),
        ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE),
    ]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


kernel32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
kernel32.Process32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32FirstW.restype = wt.BOOL
kernel32.Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32NextW.restype = wt.BOOL
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.CloseHandle.restype = wt.BOOL
kernel32.CreateProcessW.argtypes = [wt.LPCWSTR, wt.LPWSTR, ctypes.c_void_p,
                                    ctypes.c_void_p, wt.BOOL, wt.DWORD,
                                    ctypes.c_void_p, wt.LPCWSTR,
                                    ctypes.POINTER(STARTUPINFOW),
                                    ctypes.POINTER(PROCESS_INFORMATION)]
kernel32.CreateProcessW.restype = wt.BOOL
kernel32.ResumeThread.argtypes = [wt.HANDLE]
kernel32.ResumeThread.restype = wt.DWORD
kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
kernel32.WaitForSingleObject.restype = wt.DWORD
kernel32.TerminateProcess.argtypes = [wt.HANDLE, wt.UINT]
kernel32.TerminateProcess.restype = wt.BOOL


def pids_by_name(name: str) -> list[int]:
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == ctypes.c_void_p(-1).value:
        return []
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    out = []
    try:
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            if entry.szExeFile.lower() == name.lower():
                out.append(entry.th32ProcessID)
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return out


def visible_windows(pid: int) -> list[int]:
    out = []

    @EnumProc
    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            out.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return out


def park(pid: int, log=print) -> int:
    """Push every visible window off-screen and drop the process to idle."""
    hproc = kernel32.OpenProcess(PROCESS_SET_INFORMATION, False, pid)
    if hproc:
        kernel32.SetPriorityClass(hproc, IDLE_PRIORITY_CLASS)
        kernel32.CloseHandle(hproc)
    parked = 0
    for hwnd in visible_windows(pid):
        user32.ShowWindow(hwnd, SW_SHOWNOACTIVATE)
        user32.SetWindowPos(hwnd, 0, -32000, -32000, 0, 0,
                            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
        user32.ShowWindow(hwnd, SW_MINIMIZE)
        parked += 1
        log(f"parked pid={pid} hwnd=0x{hwnd:08x}")
    return parked


def _create_process(exe: str, cwd: str, args: list[str], use_desktop: bool = True,
                    log=print):
    hdesk = None
    desktop = None
    if use_desktop:
        desktop_name = f"KotORHarness-{int(time.time() * 1000) % 1000000}"
        hdesk = user32.CreateDesktopW(desktop_name, None, None, 0, GENERIC_ALL, None)
        desktop = desktop_name if hdesk else None
    if use_desktop and not hdesk:
        log(f"private desktop failed (error {ctypes.get_last_error()}); "
            f"falling back to minimized no-activate")
    cmdline = subprocess.list2cmdline([exe, *args])
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(si)
    si.dwFlags = STARTF_USESHOWWINDOW
    si.wShowWindow = SW_SHOWNOACTIVATE
    if desktop:
        si.lpDesktop = ctypes.cast(ctypes.c_wchar_p(desktop), wt.LPWSTR)
    pi = PROCESS_INFORMATION()
    flags = CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT
    ok = kernel32.CreateProcessW(None, cmdline, None, None, False, flags, None,
                                 cwd, ctypes.byref(si), ctypes.byref(pi))
    if not ok:
        if hdesk:
            user32.CloseDesktop(hdesk)
        raise OSError(ctypes.get_last_error(), f"CreateProcessW failed for {exe}")
    kernel32.SetPriorityClass(pi.hProcess, IDLE_PRIORITY_CLASS)
    return pi, hdesk, desktop


def launch_suspended(exe: str, cwd: str, args: list[str], use_desktop: bool = True,
                     log=print):
    """Create the process suspended on a private desktop.

    Returns (pid, thread_handle, process_handle, desktop_handle). The caller
    should attach/inject first, then call kernel32.ResumeThread(thread_handle).
    Close all four handles when done (and CloseDesktop on the desktop handle).
    Set use_desktop=False for applications that refuse to initialize on a
    non-interactive desktop; pair it with in-process window parking.
    """
    pi, hdesk, desktop = _create_process(exe, cwd, args, use_desktop, log)
    log(f"created suspended pid={pi.dwProcessId} desktop={desktop or '<interactive>'}")
    return pi.dwProcessId, pi.hThread, pi.hProcess, hdesk


def launch(exe: str, cwd: str, args: list[str], wait: bool,
           use_desktop: bool = True, log=print) -> int:
    """Start the process on a private desktop, windowed and idle.

    Frida callers should attach to the returned pid themselves; do not use
    frida.spawn when this launcher is used.
    """
    pi, hdesk, desktop = _create_process(exe, cwd, args, use_desktop, log)
    kernel32.ResumeThread(pi.hThread)
    log(f"launched pid={pi.dwProcessId} desktop={desktop or '<interactive>'}")
    if wait:
        kernel32.WaitForSingleObject(pi.hProcess, 0xFFFFFFFF)
    kernel32.CloseHandle(pi.hThread)
    kernel32.CloseHandle(pi.hProcess)
    if hdesk:
        user32.CloseDesktop(hdesk)
    return pi.dwProcessId


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("park")
    p.add_argument("--pid", type=int)
    p.add_argument("--name", default="swkotor.exe")

    w = sub.add_parser("watch")
    w.add_argument("--name", default="swkotor.exe")
    w.add_argument("--interval", type=float, default=1.0)
    w.add_argument("--minutes", type=float, default=60.0)

    l = sub.add_parser("launch")
    l.add_argument("--exe", required=True)
    l.add_argument("--cwd", required=True)
    l.add_argument("--wait", action="store_true")
    l.add_argument("args", nargs=argparse.REMAINDER)

    a = ap.parse_args(argv)
    if a.cmd == "park":
        pids = [a.pid] if a.pid else pids_by_name(a.name)
        for pid in pids:
            if park(pid) == 0:
                print(f"no visible windows for pid={pid}")
        return 0
    if a.cmd == "watch":
        deadline = time.monotonic() + a.minutes * 60.0
        seen = set()
        while time.monotonic() < deadline:
            for pid in pids_by_name(a.name):
                if (pid, tuple(visible_windows(pid))) not in seen:
                    n = park(pid)
                    if n:
                        seen.add((pid, tuple(visible_windows(pid))))
            time.sleep(a.interval)
        return 0
    args = a.args[1:] if a.args[:1] == ["--"] else a.args
    launch(a.exe, a.cwd, args, a.wait)
    return 0


if __name__ == "__main__":
    sys.exit(main())
