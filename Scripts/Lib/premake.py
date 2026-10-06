"""The pinned premake release: download, SHA-256 verification and running it.

premake is not committed. Scripts/Setup.py downloads the official 5.0.0 release archive for the host into
Vendor/premake/bin/ (gitignored) and verifies both the archive and the extracted executable against the SHA-256
digests below. The digests were computed from the release assets of https://github.com/premake/premake-core/releases/
tag/v5.0.0 and match the digests GitHub publishes for those assets.
"""

from __future__ import annotations

import dataclasses
import hashlib
import io
import os
import re
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Callable

from . import paths
from .process import ProcessResult, ToolNotFoundError, run_captured

PREMAKE_VERSION = "5.0.0"
RELEASE_BASE_URL = f"https://github.com/premake/premake-core/releases/download/v{PREMAKE_VERSION}/"
INSTALL_DIRECTORY = paths.VENDOR_ROOT / "premake" / "bin"

USER_AGENT = "Engine-Setup"
DOWNLOAD_ATTEMPTS = 3
DOWNLOAD_TIMEOUT_SECONDS = 60.0
MAXIMUM_ARCHIVE_BYTES = 64 * 1024 * 1024


@dataclasses.dataclass(frozen=True)
class PremakeAsset:
    archive: str  # release asset file name
    archive_sha256: str
    member: str  # the executable inside the archive
    executable_sha256: str

    @property
    def url(self) -> str:
        return RELEASE_BASE_URL + self.archive


_WINDOWS = PremakeAsset(
    "premake-5.0.0-windows.zip",
    "15e63506eed5dfd526b96c8e957a7feff2be6cfe70f46e353bfe4813b82e2fdc",
    "premake5.exe",
    "47526875051de294f4ac5bef975c97cef50699c6537a7ebd9e20743090510b04",
)

# (premake system, host machine) -> asset. The Linux build targets glibc 2.35 and runs on Ubuntu 22.04 and newer;
# premake-5.0.0-macosx.tar.gz is an arm64 binary, premake-5.0.0-macosx-x64.tar.gz the x86_64 one.
PINNED_ASSETS: dict[tuple[str, str], PremakeAsset] = {
    ("windows", "x86_64"): _WINDOWS,
    ("windows", "arm64"): _WINDOWS,  # runs under x64 emulation
    ("linux", "x86_64"): PremakeAsset(
        "premake-5.0.0-linux.tar.gz",
        "8e50e143402de3ce0f0fefe4bb3f4f6a7db46c7d66203dc9f134c0348ebfe6c5",
        "premake5",
        "e1e598d3ec583964b40fbf45f9c8d6960359cc9aadc82d0bd1869fc92890c9b4",
    ),
    ("macosx", "arm64"): PremakeAsset(
        "premake-5.0.0-macosx.tar.gz",
        "8952855a0d824f63ca22721f3f32ee947a4762c01008f9eddefeba133bafda0d",
        "premake5",
        "d67b299c0f4ccd20929a1f8befd0d8cc16ee2aba1a75eb40c1e1e3480e42bce8",
    ),
    ("macosx", "x86_64"): PremakeAsset(
        "premake-5.0.0-macosx-x64.tar.gz",
        "6005c5da9a76d980bcd13e1c9e0b0b04b53e07a39717137b2ee826429a6bda48",
        "premake5",
        "37956c12213d1c61daa440fc3c4f36f4791c64e09606a28d912375d2cf466422",
    ),
}

# Generator actions per target system, the first being the default (Architecture §2.3).
ACTIONS_BY_SYSTEM: dict[str, tuple[str, ...]] = {
    "windows": ("vs2026",),
    "linux": ("gmake", "ninja"),
    "macosx": ("xcode4", "gmake", "ninja"),
}
TOOLSETS_BY_ACTION: dict[str, tuple[str, ...]] = {
    "vs2026": ("msc", "clang"),  # clang = clang-cl (PlatformToolset ClangCL)
    "gmake": ("gcc", "clang"),
    "ninja": ("gcc", "clang"),
    "xcode4": ("clang",),
}

_GENERATED_PATTERN = re.compile(r"^Generated (.+?)\.\.\.\s*$", re.MULTILINE)


class PremakeError(Exception):
    """premake could not be installed or verified."""


def default_action(system: str) -> str:
    return ACTIONS_BY_SYSTEM[system][0]


def executable_path() -> Path:
    return INSTALL_DIRECTORY / ("premake5.exe" if os.name == "nt" else "premake5")


def pinned_asset(host: paths.Host) -> PremakeAsset:
    asset = PINNED_ASSETS.get((host.system, host.machine))
    if asset is None:
        raise PremakeError(f"no pinned premake {PREMAKE_VERSION} release binary for {host.system}/{host.machine}")
    return asset


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def installed_digest() -> str | None:
    """SHA-256 of the installed executable, or None when it is absent."""
    executable = executable_path()
    return sha256_file(executable) if executable.is_file() else None


def find_premake() -> Path:
    """The installed premake executable. Raises ToolNotFoundError naming Setup.py when it is missing."""
    executable = executable_path()
    if not executable.is_file():
        raise ToolNotFoundError(
            f"premake not found at {paths.display_path(executable)}: run python Scripts/Setup.py (it downloads the "
            f"pinned premake {PREMAKE_VERSION})"
        )
    return executable


def _download(url: str, log: Callable[[str], None]) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    last_error = ""
    for attempt in range(1, DOWNLOAD_ATTEMPTS + 1):
        try:
            with urllib.request.urlopen(request, timeout=DOWNLOAD_TIMEOUT_SECONDS) as response:
                buffer = io.BytesIO()
                while True:
                    block = response.read(1 << 16)
                    if not block:
                        break
                    buffer.write(block)
                    if buffer.tell() > MAXIMUM_ARCHIVE_BYTES:
                        raise PremakeError(f"{url} is larger than {MAXIMUM_ARCHIVE_BYTES} bytes; refusing it")
                return buffer.getvalue()
        except urllib.error.HTTPError as error:
            last_error = f"HTTP {error.code} {error.reason}"
            if error.code < 500:
                break  # a missing asset does not appear on retry
        except (urllib.error.URLError, TimeoutError, ConnectionError) as error:
            last_error = str(getattr(error, "reason", error))
        if attempt < DOWNLOAD_ATTEMPTS:
            log(f"download attempt {attempt} failed ({last_error}); retrying")
            time.sleep(2.0 * attempt)
    raise PremakeError(f"could not download {url}: {last_error}")


def _extract_member(archive_name: str, data: bytes, member: str) -> bytes:
    """Read exactly one regular file named `member` (no directories, no links) from a .zip or .tar.gz archive."""
    try:
        if archive_name.endswith(".zip"):
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                info = archive.getinfo(member)
                if info.is_dir():
                    raise PremakeError(f"{archive_name}: {member} is not a file")
                return archive.read(info)
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            info = archive.getmember(member)
            if not info.isfile():
                raise PremakeError(f"{archive_name}: {member} is not a regular file")
            stream = archive.extractfile(info)
            if stream is None:
                raise PremakeError(f"{archive_name}: cannot read {member}")
            return stream.read()
    except KeyError:
        raise PremakeError(f"{archive_name} does not contain {member}") from None
    except (zipfile.BadZipFile, tarfile.TarError, OSError) as error:
        raise PremakeError(f"{archive_name} is not a valid archive: {error}") from None


def install(host: paths.Host, log: Callable[[str], None]) -> Path:
    """Download, verify and install the pinned premake for `host`. Returns the installed executable."""
    asset = pinned_asset(host)
    log(f"downloading {asset.url}")
    data = _download(asset.url, log)
    digest = sha256_bytes(data)
    if digest != asset.archive_sha256:
        raise PremakeError(
            f"SHA-256 mismatch for {asset.archive}: expected {asset.archive_sha256}, got {digest}; the download is "
            f"corrupt or not the official release"
        )
    executable_bytes = _extract_member(asset.archive, data, asset.member)
    executable_digest = sha256_bytes(executable_bytes)
    if executable_digest != asset.executable_sha256:
        raise PremakeError(
            f"SHA-256 mismatch for {asset.member} in {asset.archive}: expected {asset.executable_sha256}, got "
            f"{executable_digest}"
        )

    INSTALL_DIRECTORY.mkdir(parents=True, exist_ok=True)
    target = executable_path()
    descriptor, temporary_name = tempfile.mkstemp(prefix=".premake5-", dir=INSTALL_DIRECTORY)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(executable_bytes)
        temporary.chmod(0o755)  # the release tarballs store the executable without the execute bit
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def run(arguments: list[str], timeout: float = 300.0, premake: Path | None = None) -> ProcessResult:
    """Run premake with `arguments` from the repository root and capture its output."""
    executable = premake or find_premake()
    return run_captured([str(executable), *arguments], cwd=paths.REPOSITORY_ROOT, timeout=timeout)


def version(premake: Path | None = None) -> str:
    result = run(["--version"], timeout=60, premake=premake)
    match = re.search(r"(\d+\.\d+\.\d+\S*)", result.output)
    if not result.succeeded or not match:
        raise PremakeError(f"'premake5 --version' failed ({result.describe_exit()}): {result.tail(3)}")
    return match.group(1)


def require_pinned(override: Path | None = None) -> Path:
    """The premake executable to generate with: `override` when given, else the installed one. Either way it must
    report the pinned version, so that every script generates with the premake the build uses.

    Raises ToolNotFoundError (missing; run Setup.py) or PremakeError (another version)."""
    executable = override if override is not None else find_premake()
    if not executable.is_file():
        raise ToolNotFoundError(f"premake not found at {executable}")
    found = version(executable)
    if found != PREMAKE_VERSION:
        raise PremakeError(
            f"{executable} is premake {found}; the pinned {PREMAKE_VERSION} is required (python Scripts/Setup.py "
            f"installs it into {paths.display_path(INSTALL_DIRECTORY)})"
        )
    return executable


def generated_files(output: str) -> list[str]:
    """The files premake reported as generated ("Generated <path>..." lines), in output order."""
    return [match.group(1).strip() for match in _GENERATED_PATTERN.finditer(output)]
