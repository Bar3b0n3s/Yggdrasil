"""Shared helpers for the developer scripts in Scripts/ (Docs/Architecture.md §2.3).

Modules:
  paths      repository layout, the workspace name (from premake5.lua), build configurations and the host platform
  process    running tools with streamed output, timeouts and process-tree termination
  report     step results, the human-readable summary, --json output and JUnit summaries
  toolchain  pinned versions and toolchain discovery (vswhere, MSVC, LLVM tools, compilers, Xcode, Vulkan SDK)
  premake    the pinned premake 5.0.0 release (download, SHA-256 verification, running it)
  workspace  reading the generated workspaces (.slnx, Makefile, build.ninja, .xcworkspace)
  scripts    running the other developer scripts as steps, and the static checks CI.py and PreCommit.py share

Everything here uses the Python standard library only (Python 3.10+).
"""
