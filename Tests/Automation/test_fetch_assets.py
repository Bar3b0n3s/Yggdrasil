"""Scripts/FetchAssets.py fixture (Docs/Architecture.md §2.3; Docs/Decisions/0022-m14-contract.md decision 16): the
pinned format fixtures under Tests/Data/Assets verify against FIXTURE_FILES and their Tests/Data/LICENSES.md mirror,
offline; and the pinned kinds of M22's Noto fonts (Docs/Decisions/0033-m22-contract.md decision 42), a member of a
GitHub release archive and a repository file at a tag, verify what they download.

The failure cases run against a temporary copy of the repository's pins (the script resolves every pinned path below
Lib.paths.REPOSITORY_ROOT), so nothing in the checkout is written and nothing is downloaded: the kinds' cases serve
their downloads from bytes built in the test.
"""

from __future__ import annotations

import dataclasses
import hashlib
import io
import shutil
import sys
import tempfile
import unittest
import zipfile
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
        noto = ("NotoSansArabic-Regular.ttf", "NotoSansHebrew-Regular.ttf", "NotoSansJP-Regular.otf")
        for font in ("Inter-Regular.otf",) + noto:
            self.assertIn(f"Tests/Data/Assets/Fonts/{font}", destinations)
        self.assertEqual(len(set(destinations)), len(destinations))
        for resource in FetchAssets.FIXTURE_FILES:
            with self.subTest(resource.destination):
                self.assertTrue(resource.destination.startswith("Tests/Data/Assets/"))
                self.assertRegex(resource.sha256, r"^[0-9a-f]{64}$")
                name = Path(resource.destination).name
                if resource.kind == "commons":
                    FetchAssets.check_url(resource.url, frozenset({FetchAssets.WIKIMEDIA_UPLOAD_HOST}))
                elif resource.kind == "release":
                    FetchAssets.check_url(resource.url, FetchAssets.GITHUB_RELEASE_HOSTS)
                    self.assertRegex(resource.archive_sha256, r"^[0-9a-f]{64}$")
                    self.assertGreater(resource.archive_size, resource.size)
                    self.assertTrue(resource.member.endswith(f"/{name}"))
                elif resource.kind == "raw":
                    FetchAssets.check_url(resource.url, frozenset({FetchAssets.GITHUB_RAW_HOST}))
                    self.assertTrue(resource.url.endswith(f"/{name}"))
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


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class FetchAssetsPinnedKindTests(unittest.TestCase):
    """The kinds "release" (a member of a GitHub release archive) and "raw" (a repository file at a tag): every download
    is served from bytes built here (FetchAssets.download replaced), so nothing is fetched."""

    MEMBER = "Font/unhinted/ttf/Font-Regular.ttf"
    FONT = b"\x00\x01\x00\x00 a font's bytes"

    @staticmethod
    def make_archive(members: dict[str, bytes]) -> bytes:
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            for name, data in members.items():
                archive.writestr(name, data)
        return buffer.getvalue()

    def release(self, archive: bytes) -> FetchAssets.PinnedResource:
        return FetchAssets.PinnedResource(
            destination="Tests/Data/Assets/Fonts/Font-Regular.ttf",
            kind="release",
            sha256=sha256(self.FONT),
            size=len(self.FONT),
            member=self.MEMBER,
            url="https://github.com/owner/fonts/releases/download/Font-v1.0/Font-v1.0.zip",
            archive_sha256=sha256(archive),
            archive_size=len(archive),
        )

    def test_a_release_member_is_extracted_once_its_archive_verifies(self) -> None:
        archive = self.make_archive({self.MEMBER: self.FONT, "OFL.txt": b"license"})
        resource = self.release(archive)
        with mock.patch.object(FetchAssets, "download", return_value=archive) as download:
            self.assertEqual(FetchAssets.download_pinned(resource, FetchAssets.ignore_progress), self.FONT)
        download.assert_called_once_with(resource.url, FetchAssets.GITHUB_RELEASE_HOSTS, len(archive), mock.ANY)

    def test_a_release_archive_or_member_that_is_not_the_pinned_one_fails(self) -> None:
        resource = self.release(self.make_archive({self.MEMBER: self.FONT}))
        other = self.make_archive({self.MEMBER: self.FONT + b"!"})
        with mock.patch.object(FetchAssets, "download", return_value=other):
            with self.assertRaises(FetchAssets.FetchError) as raised:
                FetchAssets.download_pinned(resource, FetchAssets.ignore_progress)
        self.assertIn(resource.archive_sha256, str(raised.exception))
        # An archive that matches its pin but holds the member at another size, other bytes of its size, or not at all.
        for members in ({self.MEMBER: self.FONT + b"!"}, {self.MEMBER: bytes(len(self.FONT))}, {"OFL.txt": b"license"}):
            changed = self.make_archive(members)
            pinned = dataclasses.replace(resource, archive_sha256=sha256(changed), archive_size=len(changed))
            with self.subTest(sorted(members)), mock.patch.object(FetchAssets, "download", return_value=changed):
                with self.assertRaises(FetchAssets.FetchError) as raised:
                    FetchAssets.download_pinned(pinned, FetchAssets.ignore_progress)
                self.assertIn(self.MEMBER, str(raised.exception))

    def test_a_raw_file_downloads_from_its_host_and_verifies(self) -> None:
        resource = FetchAssets.PinnedResource(
            destination="Tests/Data/Assets/Fonts/Font-Regular.otf",
            kind="raw",
            sha256=sha256(self.FONT),
            size=len(self.FONT),
            url=f"https://{FetchAssets.GITHUB_RAW_HOST}/owner/fonts/v1.0/Font-Regular.otf",
        )
        with mock.patch.object(FetchAssets, "download", return_value=self.FONT) as download:
            self.assertEqual(FetchAssets.download_pinned(resource, FetchAssets.ignore_progress), self.FONT)
        download.assert_called_once_with(
            resource.url, frozenset({FetchAssets.GITHUB_RAW_HOST}), len(self.FONT), mock.ANY
        )
        with mock.patch.object(FetchAssets, "download", return_value=self.FONT[:-1] + b"?"):
            with self.assertRaises(FetchAssets.FetchError) as raised:
                FetchAssets.download_pinned(resource, FetchAssets.ignore_progress)
        self.assertIn(resource.sha256, str(raised.exception))

    def test_a_raw_file_on_another_host_is_refused_before_any_request(self) -> None:
        no_request = AssertionError("a request was made")
        with mock.patch.object(FetchAssets.urllib.request, "build_opener", side_effect=no_request):
            with self.assertRaises(FetchAssets.FetchError) as raised:
                FetchAssets.download_raw("https://example.com/owner/fonts/v1.0/Font-Regular.otf", len(self.FONT),
                                         sha256(self.FONT), FetchAssets.ignore_progress)
        self.assertIn("refusing", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
