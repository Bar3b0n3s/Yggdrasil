"""Scripts/FetchAssets.py fixture (Docs/Architecture.md §2.3; Docs/Decisions/0022-m14-contract.md decision 16): the
pinned format fixtures under Tests/Data/Assets verify against FIXTURE_FILES and their Tests/Data/LICENSES.md mirror,
offline.

The failure cases run against a temporary copy of the repository's pins (the script resolves every pinned path below
Lib.paths.REPOSITORY_ROOT), so nothing in the checkout is written and nothing is downloaded.
"""

from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from harness import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

import FetchAssets  # noqa: E402  (Scripts/ is on sys.path above; FetchAssets imports its Lib package from there)

LICENSES = Path("Tests") / "Data" / "LICENSES.md"


class FetchAssetsFixtureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="FetchAssets-")).resolve()
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        for relative in [LICENSES] + [Path(resource.destination) for resource in FetchAssets.FIXTURE_FILES]:
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPOSITORY_ROOT / relative, target)

    def check_copy(self) -> list[Path]:
        """fixture --check against the temporary copy; returns the verified files."""
        with mock.patch.object(FetchAssets.paths, "REPOSITORY_ROOT", self.root), \
                mock.patch.object(FetchAssets, "FIXTURE_LICENSES_PATH", self.root / LICENSES):
            verified, written = FetchAssets.fetch_fixtures(check_only=True)
        self.assertEqual(written, [])
        return verified

    def test_the_committed_fixtures_match_their_pins_and_mirror(self) -> None:
        steps, files = FetchAssets.run_command(FetchAssets.parse_arguments(["fixture", "--check"]),
                                               FetchAssets.Console(True))
        self.assertEqual([step.status for step in steps], [FetchAssets.Status.PASSED], steps[0].detail)
        self.assertEqual(sorted(file.relative_to(REPOSITORY_ROOT).as_posix() for file in files),
                         sorted(resource.destination for resource in FetchAssets.FIXTURE_FILES))
        self.assertEqual(len(self.check_copy()), len(FetchAssets.FIXTURE_FILES))

    def test_every_pin_names_a_fixture_under_tests_data_with_its_own_host(self) -> None:
        destinations = [resource.destination for resource in FetchAssets.FIXTURE_FILES]
        self.assertIn("Tests/Data/Assets/Fonts/Inter-Regular.otf", destinations)
        self.assertEqual(len(set(destinations)), len(destinations))
        for resource in FetchAssets.FIXTURE_FILES:
            with self.subTest(resource.destination):
                self.assertTrue(resource.destination.startswith("Tests/Data/Assets/"))
                self.assertRegex(resource.sha256, r"^[0-9a-f]{64}$")
                if resource.kind == "commons":
                    FetchAssets.check_url(resource.url, frozenset({FetchAssets.WIKIMEDIA_UPLOAD_HOST}))
                else:
                    self.assertEqual(resource.kind, "font")
                    self.assertTrue(resource.member.startswith("extras/"))

    def test_a_changed_fixture_fails_and_names_its_pin(self) -> None:
        resource = FetchAssets.FIXTURE_FILES[0]
        target = self.root / resource.destination
        target.write_bytes(target.read_bytes() + b"\0")
        with self.assertRaises(FetchAssets.FetchError) as raised:
            self.check_copy()
        self.assertIn(resource.destination, str(raised.exception))
        self.assertIn(resource.sha256, str(raised.exception))

    def test_a_missing_fixture_fails_in_check_mode_without_downloading(self) -> None:
        resource = FetchAssets.FIXTURE_FILES[-1]
        (self.root / resource.destination).unlink()
        with mock.patch.object(FetchAssets, "download", side_effect=AssertionError("check mode downloaded")):
            with self.assertRaises(FetchAssets.FetchError) as raised:
                self.check_copy()
        self.assertIn(f"cannot read {resource.destination}", str(raised.exception))

    def test_a_pin_missing_from_the_licenses_mirror_fails(self) -> None:
        resource = FetchAssets.FIXTURE_FILES[1]
        licenses = self.root / LICENSES
        text = licenses.read_text(encoding="utf-8")
        self.assertIn(resource.sha256, text)
        licenses.write_text(text.replace(resource.sha256, "0" * 64), encoding="utf-8")
        with self.assertRaises(FetchAssets.FetchError) as raised:
            self.check_copy()
        self.assertIn(f"lists {'0' * 64} for {resource.destination}", str(raised.exception))
        licenses.write_text("\n".join(line for line in text.splitlines() if resource.destination not in line),
                            encoding="utf-8")
        with self.assertRaises(FetchAssets.FetchError) as raised:
            self.check_copy()
        self.assertIn(f"does not list it for {resource.destination}", str(raised.exception))

    def test_a_changed_fixture_is_downloaded_again_outside_check_mode(self) -> None:
        resource = FetchAssets.FIXTURE_FILES[0]
        target = self.root / resource.destination
        original = target.read_bytes()
        target.write_bytes(b"corrupt")
        with mock.patch.object(FetchAssets.paths, "REPOSITORY_ROOT", self.root), \
                mock.patch.object(FetchAssets, "FIXTURE_LICENSES_PATH", self.root / LICENSES), \
                mock.patch.object(FetchAssets, "download_pinned", return_value=original) as download:
            verified, written = FetchAssets.fetch_fixtures(check_only=False)
        download.assert_called_once()
        self.assertEqual(written, [target])
        self.assertEqual(len(verified), len(FetchAssets.FIXTURE_FILES) - 1)
        self.assertEqual(target.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
