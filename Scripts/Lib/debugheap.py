"""The Windows debug heap for a child process (Docs/Decisions/0029-m18-contract.md, W18.1 stream C, R-GPUCRASH): the
hardening stage's debug-heap GPU step runs the gpu suite's Tests processes under it (Test.py --debug-heap).

A process that Windows creates under a debugger gets the debug heap: before any heap of the process exists, the loader
sets FLG_HEAP_ENABLE_TAIL_CHECK, FLG_HEAP_ENABLE_FREE_CHECK and FLG_HEAP_VALIDATE_PARAMETERS in its NtGlobalFlag
(NT_GLOBAL_FLAG_DEBUG_HEAP, 0x70), unless its environment sets _NO_DEBUG_HEAP=1, so every heap of the process, the
graphics driver's included, checks the tail of each block, fills freed blocks with a pattern and validates its calls'
parameters. start_flags() starts a child that way (DEBUG_ONLY_THIS_PROCESS, the child alone and none of its own
children), and detach_at_initial_breakpoint() then continues the child's debug events up to the loader's initial
breakpoint and detaches (DebugActiveProcessStop): from there the child runs without a debugger, so its crash handler
reports a crash as it always does (an attached debugger would be handed the unhandled exception instead),
IsDebuggerPresent answers false, and the heap flags stay for its whole life. Nothing outside the child changes: no Image
File Execution Options, no gflags and no system setting. Windows only; the standard library's ctypes only.
"""

from __future__ import annotations

import os
import subprocess
import time
from collections.abc import Mapping

# CreateProcess's flag that makes the caller the debugger of the new process alone.
DEBUG_ONLY_THIS_PROCESS = 0x00000002
# The NtGlobalFlag bits the loader sets for a process created under a debugger: FLG_HEAP_ENABLE_TAIL_CHECK (0x10),
# FLG_HEAP_ENABLE_FREE_CHECK (0x20) and FLG_HEAP_VALIDATE_PARAMETERS (0x40).
NT_GLOBAL_FLAG_DEBUG_HEAP = 0x70
# The environment variable that keeps a debugged process off the debug heap; the child never gets it.
NO_DEBUG_HEAP_VARIABLE = "_NO_DEBUG_HEAP"
# How long the child may take to reach the loader's initial breakpoint.
INITIAL_BREAKPOINT_TIMEOUT_SECONDS = 60.0

_EXCEPTION_DEBUG_EVENT = 1
_CREATE_PROCESS_DEBUG_EVENT = 3
_EXIT_PROCESS_DEBUG_EVENT = 5
_LOAD_DLL_DEBUG_EVENT = 6
_EXCEPTION_BREAKPOINT = 0x80000003
_DBG_CONTINUE = 0x00010002
_DBG_EXCEPTION_NOT_HANDLED = 0x80010001
_WAIT_SLICE_MILLISECONDS = 1000
_ERROR_SEM_TIMEOUT = 121  # WaitForDebugEvent's error when its wait slice passed without an event


def is_available() -> bool:
    """Whether this host can start a process under the debug heap (Windows)."""
    return os.name == "nt"


def start_flags() -> int:
    """The creationflags bits to add to subprocess.Popen's for a child that detach_at_initial_breakpoint() then
    frees."""
    return DEBUG_ONLY_THIS_PROCESS


def child_environment(environment: Mapping[str, str]) -> dict[str, str]:
    """`environment` without the variable that would keep the child off the debug heap."""
    return {name: value for name, value in environment.items() if name.upper() != NO_DEBUG_HEAP_VARIABLE}


def detach_at_initial_breakpoint(process: subprocess.Popen,
                                 timeout: float = INITIAL_BREAKPOINT_TIMEOUT_SECONDS) -> None:
    """Continues the debug events of `process`, started with start_flags() by the calling thread (the only thread that
    may debug it), up to the loader's initial breakpoint, then detaches, leaving it running on the debug heap. Raises
    OSError when a debug call fails, when the process exits first or when the breakpoint does not come within `timeout`
    seconds; the caller then ends the process (it is still being debugged, so it also ends with this one)."""
    if not is_available():
        raise OSError("the Windows debug heap is not available on this host")
    import ctypes  # noqa: PLC0415  (Windows only: ctypes.wintypes is imported where it is used)
    from ctypes import wintypes  # noqa: PLC0415

    class ExceptionRecord(ctypes.Structure):
        _fields_ = [("ExceptionCode", wintypes.DWORD), ("ExceptionFlags", wintypes.DWORD),
                    ("ExceptionRecord", ctypes.c_void_p), ("ExceptionAddress", ctypes.c_void_p),
                    ("NumberParameters", wintypes.DWORD), ("ExceptionInformation", ctypes.c_size_t * 15)]

    class ExceptionDebugInfo(ctypes.Structure):
        _fields_ = [("ExceptionRecord", ExceptionRecord), ("dwFirstChance", wintypes.DWORD)]

    class CreateProcessDebugInfo(ctypes.Structure):
        _fields_ = [("hFile", wintypes.HANDLE), ("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
                    ("lpBaseOfImage", ctypes.c_void_p), ("dwDebugInfoFileOffset", wintypes.DWORD),
                    ("nDebugInfoSize", wintypes.DWORD), ("lpThreadLocalBase", ctypes.c_void_p),
                    ("lpStartAddress", ctypes.c_void_p), ("lpImageName", ctypes.c_void_p), ("fUnicode", wintypes.WORD)]

    class LoadDllDebugInfo(ctypes.Structure):
        _fields_ = [("hFile", wintypes.HANDLE), ("lpBaseOfDll", ctypes.c_void_p),
                    ("dwDebugInfoFileOffset", wintypes.DWORD), ("nDebugInfoSize", wintypes.DWORD),
                    ("lpImageName", ctypes.c_void_p), ("fUnicode", wintypes.WORD)]

    class DebugEventInfo(ctypes.Union):
        # The largest member of the Win32 union is EXCEPTION_DEBUG_INFO (160 bytes on x64).
        _fields_ = [("Exception", ExceptionDebugInfo), ("CreateProcessInfo", CreateProcessDebugInfo),
                    ("LoadDll", LoadDllDebugInfo), ("Raw", ctypes.c_byte * 160)]

    class DebugEvent(ctypes.Structure):
        _fields_ = [("dwDebugEventCode", wintypes.DWORD), ("dwProcessId", wintypes.DWORD),
                    ("dwThreadId", wintypes.DWORD), ("u", DebugEventInfo)]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.WaitForDebugEvent.argtypes = [ctypes.POINTER(DebugEvent), wintypes.DWORD]
    kernel32.WaitForDebugEvent.restype = wintypes.BOOL
    kernel32.ContinueDebugEvent.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.DWORD]
    kernel32.ContinueDebugEvent.restype = wintypes.BOOL
    kernel32.DebugSetProcessKillOnExit.argtypes = [wintypes.BOOL]
    kernel32.DebugSetProcessKillOnExit.restype = wintypes.BOOL
    kernel32.DebugActiveProcessStop.argtypes = [wintypes.DWORD]
    kernel32.DebugActiveProcessStop.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL

    def fail(call: str) -> OSError:
        error = ctypes.get_last_error()
        return OSError(error, f"{call} failed for process {process.pid}: {ctypes.FormatError(error).strip()}")

    def resume(event: DebugEvent, status: int) -> None:
        if not kernel32.ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status):
            raise fail("ContinueDebugEvent")

    deadline = time.monotonic() + timeout
    event = DebugEvent()
    while True:
        if not kernel32.WaitForDebugEvent(ctypes.byref(event), _WAIT_SLICE_MILLISECONDS):
            if ctypes.get_last_error() != _ERROR_SEM_TIMEOUT:
                raise fail("WaitForDebugEvent")
            if time.monotonic() >= deadline:
                raise OSError(f"process {process.pid} did not reach the loader's initial breakpoint within "
                              f"{timeout:.0f} s")
            continue
        code = event.dwDebugEventCode
        # The debugger owns the image file handles these events carry.
        if code == _CREATE_PROCESS_DEBUG_EVENT and event.u.CreateProcessInfo.hFile:
            kernel32.CloseHandle(event.u.CreateProcessInfo.hFile)
        elif code == _LOAD_DLL_DEBUG_EVENT and event.u.LoadDll.hFile:
            kernel32.CloseHandle(event.u.LoadDll.hFile)
        if code == _EXIT_PROCESS_DEBUG_EVENT:
            resume(event, _DBG_CONTINUE)
            raise OSError(f"process {process.pid} exited before the loader's initial breakpoint")
        if code == _EXCEPTION_DEBUG_EVENT and event.u.Exception.ExceptionRecord.ExceptionCode == _EXCEPTION_BREAKPOINT:
            # The loader's initial breakpoint: every heap the process will have takes the debug heap flags already.
            resume(event, _DBG_CONTINUE)
            if not kernel32.DebugSetProcessKillOnExit(False):
                raise fail("DebugSetProcessKillOnExit")
            if not kernel32.DebugActiveProcessStop(process.pid):
                raise fail("DebugActiveProcessStop")
            return
        # Any other exception is the process's own to handle.
        resume(event, _DBG_EXCEPTION_NOT_HANDLED if code == _EXCEPTION_DEBUG_EVENT else _DBG_CONTINUE)
