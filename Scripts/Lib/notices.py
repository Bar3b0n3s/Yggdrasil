"""THIRD_PARTY_NOTICES.txt, the notices file every exported game ships (Docs/Architecture.md §14.1, §14.2 step 6;
Docs/Decisions/0024-m15-contract.md decision 8). Stream A implements this module; Scripts/Build.py writes the file into
bin/<OutputDir>/Runtime/ after building the Runtime, and the exporter copies it into each package.

Sources:
  - each Vendor/<Lib>/VENDOR.md ends with a "## Notices" section holding one two-column table (| Field | Value |) with
    the rows Name, Upstream, Version, License (an SPDX expression), Shipped (whether exported games contain the
    library's code: "yes", "no", or "macOS" for what only macOS exports bundle), Projects (the premake projects that link
    it into the Runtime, comma-separated, or "-" for header-only code) and License files (paths relative to
    Vendor/<Lib>/, comma-separated, or "-");
  - Resources/LICENSES.md ends with a "## Notices" section holding one table with the columns Name, Upstream, Version,
    License, Shipped and License files (paths relative to Resources/, or "-" for CC0 assets, which need no notice), one
    row per engine resource in Engine.pak and per Vulkan SDK component a macOS bundle carries (the loader and MoltenVK,
    Shipped "macOS", their license texts committed under Resources/Licenses/).
A missing or malformed section is an error naming the file and the row.

Completeness is mechanical, never a hand list: every vendored library whose directory the generated Runtime or Engine
project references (referenced_vendor_libraries over their project files: include paths, links, files) must be Shipped
"yes", so a library the Runtime may link cannot ship unattributed. That over-approximates on purpose (attributing code the
linker drops, such as MikkTSpace or cgltf, is harmless; omitting shipped code is not), and one notices text serves every
configuration (Dear ImGui is "yes" because the Release Runtime keeps it for the F1 overlay, although Dist strips it). A
VENDOR.md that bundles another project's code lists that code's license among its License files (NVRHI's Vulkan-Headers).

The file (UTF-8, LF, deterministic): a first paragraph saying what follows, then one block per entry shipped on the
host (Shipped "yes", plus "macOS" on macOS) sorted by Name (case-insensitively, then byte-wise):
    ================================================================================  (80 '=')
    <Name> <Version>
    License: <License>
    Source: <Upstream>
    --------------------------------------------------------------------------------  (80 '-')
    <each license file verbatim, LF line endings, separated by one empty line>
and a single trailing newline. Entries whose License files are "-" print "No notice text is required." instead.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path

NOTICES_FILE_NAME = "THIRD_PARTY_NOTICES.txt"
NOTICES_SECTION = "Notices"


@dataclasses.dataclass(frozen=True)
class NoticeEntry:
    """One library or engine resource of a "## Notices" section."""

    name: str
    upstream: str
    version: str
    license: str  # SPDX expression
    shipped: str  # "yes", "no" or "macOS"
    projects: tuple[str, ...]  # premake projects linking it into the Runtime; empty for header-only code and resources
    license_files: tuple[Path, ...]  # absolute; empty when no notice text is required
    source: Path  # the VENDOR.md or LICENSES.md it came from


def read_notice_entries(path: Path) -> list[NoticeEntry]:
    """The entries of the "## Notices" section of `path` (a VENDOR.md: one entry; Resources/LICENSES.md: one per row).
    Raises ValueError naming the file and the row for a missing section, a missing or unknown row or column, a Shipped
    value other than yes, no or macOS, or a license file that does not exist."""
    raise NotImplementedError("contract stub: notices (ADR 0024 decision 8, stream A)")


def collect_notice_entries(repository_root: Path) -> list[NoticeEntry]:
    """Every entry of Vendor/*/VENDOR.md (the premake tool's directory has none and is skipped) and of
    Resources/LICENSES.md, sorted as the file orders them. Raises ValueError as read_notice_entries."""
    raise NotImplementedError("contract stub: notices (ADR 0024 decision 8, stream A)")


def referenced_vendor_libraries(project_files: list[Path]) -> set[str]:
    """The names of the Vendor/<Lib> directories that the generated project files `project_files` reference anywhere
    (any path holding a Vendor/<Lib>/ segment, with forward or backward slashes, or ending in Vendor/<Lib>), the
    premake tool's directory excluded. Raises OSError for a file that does not read."""
    raise NotImplementedError("contract stub: notices (ADR 0024 decision 8, stream A)")


def render_notices(entries: list[NoticeEntry], system: str) -> str:
    """The notices text of the entries among `entries` that exports on `system` ("windows", "linux" or "macosx", as
    Lib/paths.py names hosts) ship (see the module docstring)."""
    raise NotImplementedError("contract stub: notices (ADR 0024 decision 8, stream A)")


def generate_notices(repository_root: Path, system: str) -> str:
    """render_notices(collect_notice_entries(repository_root), system)."""
    raise NotImplementedError("contract stub: notices (ADR 0024 decision 8, stream A)")
