"""THIRD_PARTY_NOTICES.txt (Docs/Architecture.md §14.1; Docs/Decisions/0024-m15-contract.md decision 8, stream A):
Scripts/Lib/notices.py over the repository's VENDOR.md files and Resources/LICENSES.md, and over synthetic sections; the
completeness test generates the Windows workspace into a temporary directory with the pinned premake. These tests need no
editor build.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

from Lib import notices, paths, premake  # noqa: E402  (Scripts/ is on sys.path above)

SECTION = """# Example

Text about the library.

## Notices

| Field | Value |
|---|---|
| Name | Example Library |
| Upstream | https://example.org/example |
| Version | 1.2.3 |
| License | MIT |
| Shipped | yes |
| Projects | Example |
| License files | LICENSE |
"""


class NoticesTests(unittest.TestCase):
    @unittest.skip("contract stub: notices (ADR 0024 decision 8, stream A)")
    def test_a_vendor_section_reads_into_one_entry(self) -> None:
        with tempfile.TemporaryDirectory(prefix="Notices-") as directory:
            library = Path(directory)
            (library / "VENDOR.md").write_text(SECTION, encoding="utf-8")
            (library / "LICENSE").write_text("MIT License text\n", encoding="utf-8")
            entries = notices.read_notice_entries(library / "VENDOR.md")
            self.assertEqual(len(entries), 1)
            entry = entries[0]
            self.assertEqual((entry.name, entry.version, entry.license, entry.shipped), ("Example Library", "1.2.3", "MIT", "yes"))
            self.assertEqual(entry.projects, ("Example",))
            self.assertEqual(entry.license_files, ((library / "LICENSE").resolve(),))
            text = notices.render_notices(entries, "windows")
            self.assertIn("Example Library 1.2.3\nLicense: MIT\nSource: https://example.org/example\n", text)
            self.assertIn("MIT License text\n", text)
            self.assertTrue(text.endswith("\n") and not text.endswith("\n\n"))
            self.assertNotIn("\r", text)

    @unittest.skip("contract stub: notices (ADR 0024 decision 8, stream A)")
    def test_a_missing_or_malformed_section_names_the_file(self) -> None:
        with tempfile.TemporaryDirectory(prefix="Notices-") as directory:
            library = Path(directory)
            (library / "VENDOR.md").write_text("# Example\n\nNo notices here.\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "VENDOR.md"):
                notices.read_notice_entries(library / "VENDOR.md")
            (library / "VENDOR.md").write_text(SECTION.replace("| Shipped | yes |", "| Shipped | maybe |"), encoding="utf-8")
            (library / "LICENSE").write_text("text\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Shipped"):
                notices.read_notice_entries(library / "VENDOR.md")
            (library / "VENDOR.md").write_text(SECTION.replace("| License files | LICENSE |", "| License files | MISSING |"),
                                                encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "MISSING"):
                notices.read_notice_entries(library / "VENDOR.md")

    @unittest.skip("contract stub: notices (ADR 0024 decision 8, stream A)")
    def test_the_repository_notices_cover_every_library_the_runtime_references_and_the_inter_license(self) -> None:
        entries = notices.collect_notice_entries(REPOSITORY_ROOT)
        vendored = sorted(path.parent.name for path in (REPOSITORY_ROOT / "Vendor").glob("*/VENDOR.md"))
        sources = {entry.source.parent.name: entry for entry in entries if entry.source.name == "VENDOR.md"}
        self.assertEqual(sorted(sources), vendored)
        # Every library the generated Runtime or Engine project references ships attributed (decision 8): derived from a
        # fresh generation, never a hand list.
        with tempfile.TemporaryDirectory(prefix="NoticesWorkspace-") as directory:
            location = Path(directory)
            subprocess.run([str(premake.find_premake()), "--fatal", "--os=windows", f"--to={location}", "vs2026"],
                           cwd=paths.REPOSITORY_ROOT, capture_output=True, check=True, timeout=300.0)
            referenced = notices.referenced_vendor_libraries([location / "Runtime.vcxproj", location / "Engine.vcxproj"])
        for library in ("imgui", "JoltPhysics", "Luau", "NVRHI", "miniaudio", "spdlog", "GLFW", "glm", "entt", "json", "stb"):
            self.assertIn(library, referenced)
        self.assertNotIn("doctest", referenced)
        for library in sorted(referenced):
            with self.subTest(library=library):
                self.assertEqual(sources[library].shipped, "yes")
        self.assertTrue(any("Vulkan-Headers" in str(path) for path in sources["NVRHI"].license_files))
        self.assertEqual(sources["doctest"].shipped, "no")
        self.assertIn("Inter", {entry.name for entry in entries if entry.shipped == "yes"})
        self.assertEqual(sorted(entry.name for entry in entries if entry.shipped == "macOS"), ["MoltenVK", "Vulkan Loader"])
        text = notices.generate_notices(REPOSITORY_ROOT, "windows")
        self.assertEqual(text, notices.generate_notices(REPOSITORY_ROOT, "windows"))
        self.assertIn("SIL OPEN FONT LICENSE", text.upper())
        self.assertNotIn("MoltenVK", text)
        self.assertIn("MoltenVK", notices.generate_notices(REPOSITORY_ROOT, "macosx"))


if __name__ == "__main__":
    unittest.main()
