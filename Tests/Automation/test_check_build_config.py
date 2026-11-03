"""Scripts/CheckBuildConfig.py's consumer rule (its rule 1): which projects include a vendored library's headers.

A consumer is a project whose include path holds one of the library's own include directories, those below the
Vendor/<Lib> directory of its sources. A directory the library borrows from another one holds that other library's
headers (M23: miniaudio's project borrows the include directories of libogg, libopus and opusfile for its libopus
decoding backend, Docs/Decisions/0034-m23-contract.md decision 16), so sharing it makes no consumer of the borrower.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import CheckBuildConfig  # noqa: E402  (Scripts/ is on sys.path above)
from Lib import paths  # noqa: E402

CONFIG = "Debug"


def make_project(name: str, sources: list[Path], includes: list[Path]) -> CheckBuildConfig.Project:
    return CheckBuildConfig.Project(name=name, sources=sources,
                                    include_dirs={CONFIG: {CheckBuildConfig.normalize_path(path) for path in includes}})


def consumer_names(vendor: CheckBuildConfig.Project, projects: list[CheckBuildConfig.Project]) -> set[str]:
    return {project.name for project in CheckBuildConfig.consumers_of(vendor, {p.name: p for p in projects}, CONFIG)}


class CheckBuildConfigConsumerTests(unittest.TestCase):
    def test_a_borrowed_include_directory_makes_no_consumer_of_the_borrower(self) -> None:
        vendor = paths.VENDOR_ROOT
        miniaudio = make_project("miniaudio",
                                 [vendor / "miniaudio/miniaudio_vorbis.c",
                                  vendor / "miniaudio/extras/decoders/libopus/miniaudio_libopus.c"],
                                 [vendor / "miniaudio", vendor / "stb", vendor / "libogg/include"])
        ogg = make_project("ogg", [vendor / "libogg/src/framing.c"], [vendor / "libogg/include"])
        engine = make_project("Engine", [REPOSITORY_ROOT / "Engine/Source/Engine/Audio/AudioDecoder.cpp"],
                              [vendor / "miniaudio"])
        projects = [miniaudio, ogg, engine]
        # libogg's include directory is miniaudio's only by borrowing: libogg is no consumer of miniaudio.
        self.assertEqual(consumer_names(miniaudio, projects), {"Engine"})
        # miniaudio does include libogg's headers.
        self.assertEqual(consumer_names(ogg, projects), {"miniaudio"})

    def test_a_library_whose_sources_span_libraries_keeps_every_include_directory(self) -> None:
        vendor = paths.VENDOR_ROOT
        spanning = make_project("Spanning", [vendor / "first/a.c", vendor / "second/b.c"],
                                [vendor / "first/include", vendor / "third/include"])
        user = make_project("User", [REPOSITORY_ROOT / "Engine/Source/Engine/Core/Log.cpp"], [vendor / "third/include"])
        self.assertEqual(consumer_names(spanning, [spanning, user]), {"User"})


if __name__ == "__main__":
    unittest.main()
