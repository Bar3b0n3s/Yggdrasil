#!/usr/bin/env python3
"""Download the approved third-party assets from their official sources (Docs/Architecture.md §2.3, Roadmap M6).

Commands:
  hdri <id> --res 1k|2k|4k --dest <dir>
      A Poly Haven HDRI in Radiance HDR format. The file list comes from https://api.polyhaven.com/files/<id> and the
      file from dl.polyhaven.org; no other host is contacted, redirects to another host fail the download, and the
      downloaded bytes must match the size and md5 that the API lists. Written as <dest>/<id>_<res>.hdr.
  font --dest <dir>
      The pinned Inter release (Inter-Regular.ttf from the github.com/rsms/inter release archive), verified against
      its pinned SHA-256: the archive first, then the extracted font. Written as <dest>/Inter-Regular.ttf.
  defaults [--check]
      The committed engine resources (Docs/Decisions/0001-approvals.md): studio_small_09 and
      kloofendal_48d_partly_cloudy_puresky at 1k as Resources/Environments/Studio.hdr and Sky.hdr, and Inter-Regular.ttf
      as Resources/Fonts/Inter-Regular.ttf, each verified against the SHA-256 and size pinned below (DEFAULT_RESOURCES),
      which Resources/LICENSES.md mirrors (a pin missing there, or different, fails the command). --check downloads
      nothing: it verifies the committed files and the mirror only.
The `fixture` command (the pinned FLAC, MP3 and OTF format fixtures, Architecture §15.5) joins with Roadmap M14.

Downloads need operator approval, which Roadmap M0 collects up front; the items this script fetches are the ones
recorded in Docs/Decisions/0001-approvals.md. Files are written to a temporary name and renamed into place only after
verification, so a failed download leaves nothing behind. Poly Haven asks API clients to identify themselves, so every
request carries the User-Agent below.

Exit codes: 0 success, 1 a download failed or did not verify, 2 usage error.
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"FetchAssets.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import dataclasses
import hashlib
import io
import json
import os
import re
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Callable

from Lib import paths
from Lib.report import Console, Status, Step, configure_stdio, emit_json, overall_exit_code

POLY_HAVEN_API_HOST = "api.polyhaven.com"
POLY_HAVEN_DOWNLOAD_HOST = "dl.polyhaven.org"
HDRI_RESOLUTIONS = ("1k", "2k", "4k")
# Poly Haven asset ids are lowercase words joined by underscores ("kloofendal_48d_partly_cloudy_puresky").
POLY_HAVEN_ID_PATTERN = re.compile(r"^[a-z0-9]+(?:_[a-z0-9]+)*$")
# A Radiance HDR file starts with this signature ("#?RGBE" in some writers).
RADIANCE_SIGNATURES = (b"#?RADIANCE", b"#?RGBE")

# The pinned Inter release. The digests were computed from the official release asset
# https://github.com/rsms/inter/releases/tag/v4.1 when the font was first committed (Roadmap M6).
INTER_VERSION = "4.1"
INTER_ARCHIVE_URL = f"https://github.com/rsms/inter/releases/download/v{INTER_VERSION}/Inter-{INTER_VERSION}.zip"
INTER_ARCHIVE_SHA256 = "9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e"
INTER_ARCHIVE_SIZE = 33707794
INTER_ARCHIVE_MEMBER = "extras/ttf/Inter-Regular.ttf"
INTER_FONT_NAME = "Inter-Regular.ttf"
INTER_FONT_SHA256 = "40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82"
INTER_FONT_SIZE = 411640
# github.com answers a release download with a redirect to its asset host.
GITHUB_RELEASE_HOSTS = frozenset(
    {"github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"}
)

USER_AGENT = "Engine-FetchAssets/1.0"
DOWNLOAD_ATTEMPTS = 3
DOWNLOAD_TIMEOUT_SECONDS = 120.0
MAXIMUM_API_BYTES = 1024 * 1024
MAXIMUM_DOWNLOAD_BYTES = 512 * 1024 * 1024

LICENSES_PATH = paths.REPOSITORY_ROOT / "Resources" / "LICENSES.md"
# A pin in Resources/LICENSES.md: a table row naming the repository path and the SHA-256, both in backticks.
LICENSE_PIN_PATTERN = re.compile(r"`(Resources/[^`]+)`.*?`([0-9a-f]{64})`")


class FetchError(Exception):
    """A download that failed, was refused or did not verify."""


@dataclasses.dataclass(frozen=True)
class PinnedResource:
    destination: str  # repository-relative path of the committed file
    kind: str  # "hdri" or "font"
    sha256: str
    size: int
    asset_id: str = ""  # kind "hdri": the Poly Haven id
    resolution: str = ""  # kind "hdri": one of HDRI_RESOLUTIONS

    @property
    def path(self) -> Path:
        return paths.REPOSITORY_ROOT / self.destination

    @property
    def label(self) -> str:
        return f"{self.asset_id} {self.resolution}" if self.kind == "hdri" else f"Inter {INTER_VERSION}"


# The committed engine resources (Resources/EngineAssets.json names them). The HDRI digests were computed from the
# Poly Haven 1k .hdr files when they were first committed (Roadmap M6), after their size and md5 matched the API.
DEFAULT_RESOURCES = (
    PinnedResource(
        destination="Resources/Environments/Studio.hdr",
        kind="hdri",
        sha256="e7cfda5f4e98e623db12b8bfd0184e048488e4855d9c83e2751fb44a32e80c45",
        size=1615248,
        asset_id="studio_small_09",
        resolution="1k",
    ),
    PinnedResource(
        destination="Resources/Environments/Sky.hdr",
        kind="hdri",
        sha256="fd94c84997b8a3c353b62c2125a9b44e19509956986a126e472684432a02d798",
        size=1435119,
        asset_id="kloofendal_48d_partly_cloudy_puresky",
        resolution="1k",
    ),
    PinnedResource(
        destination="Resources/Fonts/Inter-Regular.ttf",
        kind="font",
        sha256=INTER_FONT_SHA256,
        size=INTER_FONT_SIZE,
    ),
)


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Download the approved third-party assets from official sources.",
        epilog="Exit codes: 0 success, 1 a download failed or did not verify, 2 usage error.",
    )
    parser.add_argument("--json", action="store_true", help="print a machine-readable report on stdout")
    commands = parser.add_subparsers(dest="command", required=True)
    hdri = commands.add_parser("hdri", help="a Poly Haven HDRI (md5-verified)")
    hdri.add_argument("id", help="the Poly Haven asset id, for example studio_small_09")
    hdri.add_argument("--res", choices=HDRI_RESOLUTIONS, default="1k", help="the resolution (default 1k)")
    hdri.add_argument("--dest", type=Path, required=True, help="the destination directory")
    font = commands.add_parser("font", help="the pinned Inter release (SHA-256-verified)")
    font.add_argument("--dest", type=Path, required=True, help="the destination directory")
    defaults = commands.add_parser("defaults", help="the committed engine resources under Resources/")
    defaults.add_argument(
        "--check", action="store_true", help="verify the committed files against their pins; download nothing"
    )
    return parser.parse_args(arguments)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def check_url(url: str, allowed_hosts: frozenset[str]) -> None:
    """Refuse any URL that is not plain https on one of `allowed_hosts` (no credentials, no other port)."""
    refusal = FetchError(f"refusing {url}: only https on {', '.join(sorted(allowed_hosts))} is allowed")
    parsed = urllib.parse.urlsplit(url)
    try:
        port = parsed.port
    except ValueError:
        raise refusal from None
    if parsed.scheme != "https" or parsed.hostname not in allowed_hosts or parsed.username or port not in (None, 443):
        raise refusal


class RestrictedRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Follows a redirect only when its target passes check_url, so a download never leaves the approved hosts."""

    def __init__(self, allowed_hosts: frozenset[str]) -> None:
        super().__init__()
        self.allowed_hosts = allowed_hosts

    def redirect_request(
        self, req: urllib.request.Request, fp: Any, code: int, msg: str, headers: Any, newurl: str
    ) -> urllib.request.Request | None:
        check_url(newurl, self.allowed_hosts)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download(url: str, allowed_hosts: frozenset[str], maximum_bytes: int, log: Callable[[str], None]) -> bytes:
    """The body of `url`, fetched over https from `allowed_hosts` only. Server errors and timeouts are retried; a
    refused host, a client error or a body larger than `maximum_bytes` is not."""
    check_url(url, allowed_hosts)
    opener = urllib.request.build_opener(RestrictedRedirectHandler(allowed_hosts))
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    last_error = ""
    for attempt in range(1, DOWNLOAD_ATTEMPTS + 1):
        try:
            with opener.open(request, timeout=DOWNLOAD_TIMEOUT_SECONDS) as response:
                check_url(response.geturl(), allowed_hosts)
                buffer = io.BytesIO()
                while True:
                    block = response.read(1 << 16)
                    if not block:
                        break
                    buffer.write(block)
                    if buffer.tell() > maximum_bytes:
                        raise FetchError(f"{url} is larger than {maximum_bytes} bytes; refusing it")
                return buffer.getvalue()
        except urllib.error.HTTPError as error:
            last_error = f"HTTP {error.code} {error.reason}"
            if error.code < 500:
                break  # a missing or forbidden file does not appear on retry
        except (urllib.error.URLError, TimeoutError, ConnectionError) as error:
            last_error = str(getattr(error, "reason", error))
        if attempt < DOWNLOAD_ATTEMPTS:
            log(f"download attempt {attempt} of {url} failed ({last_error}); retrying")
            time.sleep(2.0 * attempt)
    raise FetchError(f"could not download {url}: {last_error}")


def write_atomically(path: Path, data: bytes) -> None:
    """Write `data` to a temporary file next to `path`, then rename it into place."""
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}-", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def download_hdri(asset_id: str, resolution: str, log: Callable[[str], None]) -> bytes:
    """The Radiance .hdr of Poly Haven HDRI `asset_id` at `resolution`, verified against the API's size and md5."""
    if not POLY_HAVEN_ID_PATTERN.match(asset_id):
        raise FetchError(f"'{asset_id}' is not a Poly Haven asset id (lowercase letters, digits and underscores)")
    if resolution not in HDRI_RESOLUTIONS:
        raise FetchError(f"'{resolution}' is not one of {', '.join(HDRI_RESOLUTIONS)}")
    api_url = f"https://{POLY_HAVEN_API_HOST}/files/{asset_id}"
    log(f"reading {api_url}")
    listing = download(api_url, frozenset({POLY_HAVEN_API_HOST}), MAXIMUM_API_BYTES, log)
    try:
        entry = json.loads(listing)["hdri"][resolution]["hdr"]
        url = entry["url"]
        size = entry["size"]
        md5 = entry["md5"]
    except (ValueError, KeyError, TypeError) as error:
        raise FetchError(f"{api_url} lists no {resolution} .hdr file for '{asset_id}' ({error!r})") from None
    if not isinstance(url, str) or not isinstance(size, int) or not isinstance(md5, str) or size <= 0:
        raise FetchError(f"{api_url} lists a malformed {resolution} .hdr entry")

    log(f"downloading {url}")
    data = download(url, frozenset({POLY_HAVEN_DOWNLOAD_HOST}), min(size, MAXIMUM_DOWNLOAD_BYTES), log)
    if len(data) != size:
        raise FetchError(f"{url}: got {len(data)} bytes, the API lists {size}")
    digest = hashlib.md5(data).hexdigest()
    if digest != md5.lower():
        raise FetchError(f"md5 mismatch for {url}: the API lists {md5}, got {digest}")
    if not data.startswith(RADIANCE_SIGNATURES):
        raise FetchError(f"{url} is not a Radiance HDR file")
    return data


def download_font(log: Callable[[str], None]) -> bytes:
    """The pinned Inter-Regular.ttf, extracted from the pinned release archive; both verified against their SHA-256."""
    log(f"downloading {INTER_ARCHIVE_URL}")
    archive = download(INTER_ARCHIVE_URL, GITHUB_RELEASE_HOSTS, INTER_ARCHIVE_SIZE, log)
    digest = sha256_bytes(archive)
    if len(archive) != INTER_ARCHIVE_SIZE or digest != INTER_ARCHIVE_SHA256:
        raise FetchError(
            f"SHA-256 mismatch for {INTER_ARCHIVE_URL}: expected {INTER_ARCHIVE_SHA256} ({INTER_ARCHIVE_SIZE} "
            f"bytes), got {digest} ({len(archive)} bytes); the download is corrupt or not the official release"
        )
    try:
        with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
            info = bundle.getinfo(INTER_ARCHIVE_MEMBER)
            if info.is_dir() or info.file_size != INTER_FONT_SIZE:
                raise FetchError(
                    f"{INTER_ARCHIVE_MEMBER} in the Inter archive is not the pinned {INTER_FONT_SIZE}-byte file"
                )
            font = bundle.read(info)
    except KeyError:
        raise FetchError(f"the Inter archive does not contain {INTER_ARCHIVE_MEMBER}") from None
    except (zipfile.BadZipFile, OSError) as error:
        raise FetchError(f"the Inter archive is not a valid zip file: {error}") from None
    font_digest = sha256_bytes(font)
    if font_digest != INTER_FONT_SHA256:
        raise FetchError(
            f"SHA-256 mismatch for {INTER_ARCHIVE_MEMBER}: expected {INTER_FONT_SHA256}, got {font_digest}"
        )
    return font


def ignore_progress(message: str) -> None:
    """The default progress callback: library callers that pass none hear nothing."""


def fetch_hdri(
    asset_id: str, resolution: str, destination: Path, log: Callable[[str], None] = ignore_progress
) -> Path:
    """Downloads one Poly Haven HDRI into `destination` and returns the written file."""
    data = download_hdri(asset_id, resolution, log)
    target = destination / f"{asset_id}_{resolution}.hdr"
    write_atomically(target, data)
    return target


def fetch_font(destination: Path, log: Callable[[str], None] = ignore_progress) -> Path:
    """Downloads the pinned Inter-Regular.ttf into `destination` and returns the written file."""
    data = download_font(log)
    target = destination / INTER_FONT_NAME
    write_atomically(target, data)
    return target


def mirrored_pins() -> dict[str, str]:
    """The SHA-256 pins Resources/LICENSES.md lists, by repository path."""
    try:
        text = LICENSES_PATH.read_text(encoding="utf-8")
    except OSError as error:
        raise FetchError(f"cannot read {paths.display_path(LICENSES_PATH)}: {error}") from None
    pins: dict[str, str] = {}
    for line in text.splitlines():
        match = LICENSE_PIN_PATTERN.search(line)
        if match:
            pins[match.group(1)] = match.group(2)
    return pins


def check_mirror(resource: PinnedResource, pins: dict[str, str]) -> None:
    mirrored = pins.get(resource.destination)
    if mirrored != resource.sha256:
        listed = f"lists {mirrored}" if mirrored else "does not list it"
        raise FetchError(
            f"{paths.display_path(LICENSES_PATH)} {listed} for {resource.destination}; the pin is {resource.sha256} "
            "(DEFAULT_RESOURCES in Scripts/FetchAssets.py)"
        )


def verify_pinned(resource: PinnedResource, data: bytes) -> None:
    digest = sha256_bytes(data)
    if len(data) != resource.size or digest != resource.sha256:
        raise FetchError(
            f"{resource.destination}: expected SHA-256 {resource.sha256} ({resource.size} bytes), got {digest} "
            f"({len(data)} bytes)"
        )


def fetch_defaults(check_only: bool = False, log: Callable[[str], None] = ignore_progress) -> list[Path]:
    """Downloads the committed engine resources into Resources/ (or, with `check_only`, verifies the committed
    files) and returns the files."""
    pins = mirrored_pins()
    for resource in DEFAULT_RESOURCES:
        check_mirror(resource, pins)
    written: list[Path] = []
    for resource in DEFAULT_RESOURCES:
        if check_only:
            try:
                data = resource.path.read_bytes()
            except OSError as error:
                raise FetchError(f"cannot read {resource.destination}: {error}") from None
            verify_pinned(resource, data)
        else:
            if resource.kind == "hdri":
                data = download_hdri(resource.asset_id, resource.resolution, log)
            else:
                data = download_font(log)
            verify_pinned(resource, data)
            write_atomically(resource.path, data)
        written.append(resource.path)
    return written


def run_command(options: argparse.Namespace, console: Console) -> tuple[list[Step], list[Path]]:
    start = time.perf_counter()
    if options.command == "hdri":
        name = f"hdri {options.id} {options.res}"
    elif options.command == "font":
        name = f"font Inter {INTER_VERSION}"
    else:
        name = "defaults --check" if options.check else "defaults"
    files: list[Path] = []
    try:
        if options.command == "hdri":
            files = [fetch_hdri(options.id, options.res, options.dest, console.print)]
        elif options.command == "font":
            files = [fetch_font(options.dest, console.print)]
        else:
            files = fetch_defaults(options.check, console.print)
    except (FetchError, OSError) as error:
        return [Step(name, Status.FAILED, str(error), time.perf_counter() - start)], []
    verb = "verified" if options.command == "defaults" and options.check else "wrote"
    detail = f"{verb} " + ", ".join(paths.display_path(file) for file in files)
    return [Step(name, Status.PASSED, detail, time.perf_counter() - start)], files


def main(arguments: list[str] | None = None) -> int:
    configure_stdio()
    options = parse_arguments(sys.argv[1:] if arguments is None else arguments)
    console = Console(options.json)
    console.heading("Fetch assets")
    steps, files = run_command(options, console)
    for step in steps:
        console.result(step)
    exit_code = overall_exit_code(steps)
    if options.json:
        emit_json(
            {
                "success": exit_code == 0,
                "command": options.command,
                "files": [file.as_posix() for file in files],
                "steps": [step.to_json() for step in steps],
            }
        )
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
