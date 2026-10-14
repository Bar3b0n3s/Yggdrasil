"""The Dist map-file check's parser and families (Docs/Architecture.md §14.4; Roadmap M15 acceptance "Dist: map file
contains no automation, Analysis, ImGui or AssetPipeline symbols"; Docs/Decisions/0024-m15-contract.md decision 11,
stream D), over the seeded fixtures of Tests/Data/MapFiles/<format>/: Clean.map has no finding, and each
<Family>.map has exactly the findings its Expected.json lists. The real Dist map is checked by CI.py after its Dist
build. These tests need no editor build.
"""

from __future__ import annotations

import json
import sys
import unittest

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

from Lib import runtime_map  # noqa: E402  (Scripts/ is on sys.path above)

MAP_FIXTURES = REPOSITORY_ROOT / "Tests" / "Data" / "MapFiles"


class RuntimeMapTests(unittest.TestCase):
    @unittest.skip("contract stub: runtime map (ADR 0024 decision 11, stream D)")
    def test_every_format_parses_and_every_seeded_fixture_has_exactly_its_findings(self) -> None:
        patterns = runtime_map.family_patterns(REPOSITORY_ROOT)
        self.assertEqual(sorted(patterns), sorted(runtime_map.FORBIDDEN_FAMILIES))
        for map_format in runtime_map.MAP_FORMATS:
            directory = MAP_FIXTURES / map_format
            expected = json.loads((directory / "Expected.json").read_text(encoding="utf-8"))
            self.assertEqual(sorted(expected), sorted(["Clean", *runtime_map.FORBIDDEN_FAMILIES, "MissingRequired"]))
            for case, findings in expected.items():
                with self.subTest(format=map_format, case=case):
                    parsed = runtime_map.parse_map((directory / f"{case}.map").read_text(encoding="utf-8"))
                    self.assertEqual(parsed.format, map_format)
                    self.assertEqual(runtime_map.check_map(parsed, patterns), findings)

    @unittest.skip("contract stub: runtime map (ADR 0024 decision 11, stream D)")
    def test_string_literals_and_object_stems_never_count(self) -> None:
        text = (MAP_FIXTURES / "msvc" / "Clean.map").read_text(encoding="utf-8")
        parsed = runtime_map.parse_map(text)
        names = [symbol.name for symbol in parsed.symbols]
        self.assertTrue(any("??_C@" in name and "ImGui" in name for name in names))  # a string literal "ImGui"
        self.assertTrue(any(symbol.origin.endswith("Watchdog1.obj") for symbol in parsed.symbols))
        self.assertEqual(runtime_map.check_map(parsed, runtime_map.family_patterns(REPOSITORY_ROOT)), [])

    @unittest.skip("contract stub: runtime map (ADR 0024 decision 11, stream D)")
    def test_text_that_is_no_map_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            runtime_map.parse_map("not a map file\n")


if __name__ == "__main__":
    unittest.main()
