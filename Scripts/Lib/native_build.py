"""The native build of a game's module (Docs/Decisions/0030-m19-contract.md decision 30, C-NATIVE-STATIC): what
Scripts/BuildNative.py (stream N-A) and the games suite's native preparation (decision 35) share. The editor runs
BuildNative.py by fixed paths with fixed arguments (EditorCore/Native/NativeBuild.h); its C++ ListNativeSources and
ComputeNativeSourceHash compute what list_sources and source_hash compute here, which a test pins. Windows backends only
(Docs/Decisions/0028-backlog-program.md decision 10): the build itself runs on Windows; these helpers are pure Python.
"""

from __future__ import annotations

from pathlib import Path

# The project directory of a game's module and the kinds of file it compiles (decision 26).
NATIVE_DIRECTORY = "Native"
SOURCE_SUFFIXES = (".cpp", ".h")
# The build record every build writes into its output directory (decision 30).
RECORD_FILE = "NativeBuild.json"
RECORD_FORMAT = "NativeBuild"
RECORD_VERSION = 1
# The interface every module compiles against, repository-relative, part of the source hash.
API_HEADER = "Engine/Source/Engine/Native/NativeApi.h"


def list_sources(project_root: Path) -> list[str]:
    """Every .cpp and .h file below <project_root>/Native/, project-relative with forward slashes ("Native/Spin.cpp"),
    sorted by their UTF-8 bytes; empty when the project has no Native/ directory. Raises OSError when a directory cannot
    be listed."""
    raise NotImplementedError("contract stub: native_build (ADR 0030 decision 30, stream N-A)")


def source_hash(project_root: Path, configuration: str, api_header: Path) -> str:
    """The source hash of a module build (decision 30), as 16 lowercase hex digits: XXH64 (seed 0, Lib/xxh64.py) over
    the UTF-8 text "EngineNativeSources 1\\n<configuration>\\n", then for `api_header` and for each of list_sources'
    files in order: its path ("Engine/Native/NativeApi.h" for the header, the project-relative path for a source),
    "\\n", its size as 8 little-endian bytes and its bytes. `configuration` is "Debug", "Release" or "Dist". A build is
    current when its record's SourceHash equals the value now. Raises FileNotFoundError for a missing header."""
    raise NotImplementedError("contract stub: native_build (ADR 0030 decision 30, stream N-A)")
