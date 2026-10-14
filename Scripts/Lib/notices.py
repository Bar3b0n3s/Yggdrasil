"""THIRD_PARTY_NOTICES.txt, the notices file every exported game ships (Docs/Architecture.md §14.1, §14.2 step 6;
Docs/Decisions/0024-m15-contract.md decision 8). Scripts/Build.py writes the file into bin/<OutputDir>/Runtime/ after
building the Runtime, and the exporter copies it into each package.

Sources:
  - each Vendor/<Lib>/VENDOR.md ends with a "## Notices" section holding one two-column table (| Field | Value |) with
    the rows Name, Upstream, Version, License (an SPDX expression), Shipped (whether exported games contain the
    library's code: "yes", "no", or "macOS" for what only macOS exports bundle), Projects (the premake projects that
    link it into the Runtime, comma-separated, or "-" for header-only code) and License files (paths relative to
    Vendor/<Lib>/, comma-separated, or "-");
  - Resources/LICENSES.md ends with a "## Notices" section holding one table with the columns Name, Upstream, Version,
    License, Shipped and License files (paths relative to Resources/, or "-" for CC0 assets, which need no notice), one
    row per engine resource in Engine.pak and per Vulkan SDK component a macOS bundle carries (the loader and MoltenVK,
    Shipped "macOS", their license texts committed under Resources/Licenses/).
A missing or malformed section is an error naming the file and the row. Cells may wrap a value in backticks.

Completeness is mechanical, never a hand list: every vendored library whose directory the generated Runtime or Engine
project references (referenced_vendor_libraries over their project files: include paths, links, files) must be Shipped
"yes", so a library the Runtime may link cannot ship unattributed. That over-approximates on purpose (attributing code
the linker drops, such as MikkTSpace or cgltf, is harmless; omitting shipped code is not), and one notices text serves
every configuration (Dear ImGui is "yes" because the Release Runtime keeps it for the F1 overlay, although Dist strips
it). A VENDOR.md that bundles another project's code lists that code's license among its License files (NVRHI's
Vulkan-Headers).

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
import re
from pathlib import Path

NOTICES_FILE_NAME = "THIRD_PARTY_NOTICES.txt"
NOTICES_SECTION = "Notices"

# The rows of a VENDOR.md section, in the order the table lists them, and the columns of Resources/LICENSES.md's.
VENDOR_FIELDS = ("Name", "Upstream", "Version", "License", "Shipped", "Projects", "License files")
RESOURCE_COLUMNS = ("Name", "Upstream", "Version", "License", "Shipped", "License files")
SHIPPED_VALUES = ("yes", "no", "macOS")
# The vendored premake tool builds the workspace and ships nowhere; its directory has no VENDOR.md.
PREMAKE_DIRECTORY = "premake"
RULE_WIDTH = 80
INTRODUCTION = ("This game is built with third-party software and assets. Their notices and license texts follow, one "
                "block per component.")
NO_NOTICE_TEXT = "No notice text is required."
HEADING_PATTERN = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
SEPARATOR_PATTERN = re.compile(r"^\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)*\|?\s*$")
# A Vendor/<Lib> segment in a generated project file: Windows or POSIX separators, absolute or relative.
VENDOR_REFERENCE_PATTERN = re.compile(r"(?<![A-Za-z0-9_.])Vendor[\\/]+([A-Za-z0-9_.+-]+)")


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


def _section_lines(path: Path, text: str) -> list[str]:
    """The lines of the "## Notices" section of `text` (up to the next heading of level 1 or 2)."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = HEADING_PATTERN.match(line)
        if match and len(match.group(1)) == 2 and match.group(2) == NOTICES_SECTION:
            section: list[str] = []
            for following in lines[index + 1:]:
                heading = HEADING_PATTERN.match(following)
                if heading and len(heading.group(1)) <= 2:
                    break
                section.append(following)
            return section
    raise ValueError(f"{path}: no '## {NOTICES_SECTION}' section")


def _split_row(line: str) -> list[str]:
    """The cells of one Markdown table row, trimmed."""
    stripped = line.strip()
    if stripped.startswith("|"):
        stripped = stripped[1:]
    if stripped.endswith("|"):
        stripped = stripped[:-1]
    return [cell.strip() for cell in stripped.split("|")]


def _unquote(value: str) -> str:
    """A cell value without the backticks that may wrap it."""
    value = value.strip()
    if len(value) >= 2 and value.startswith("`") and value.endswith("`"):
        return value[1:-1].strip()
    return value


def _read_table(path: Path, section: list[str]) -> tuple[list[str], list[list[str]]]:
    """The header cells and the data rows of the first table of the section."""
    rows = [line for line in section if line.strip().startswith("|")]
    if len(rows) < 2 or not SEPARATOR_PATTERN.match(rows[1].strip()):
        raise ValueError(f"{path}: the '## {NOTICES_SECTION}' section holds no table")
    header = _split_row(rows[0])
    data = [_split_row(row) for row in rows[2:]]
    for number, cells in enumerate(data, start=1):
        if len(cells) != len(header):
            raise ValueError(f"{path}: row {number} of the '## {NOTICES_SECTION}' table has {len(cells)} cells, "
                             f"not {len(header)}")
    return header, data


def _list_value(value: str) -> list[str]:
    """A comma-separated cell as its items; "-" is none."""
    value = _unquote(value)
    if value in ("-", ""):
        return []
    return [_unquote(item) for item in value.split(",") if item.strip()]


def _make_entry(path: Path, row_name: str, fields: dict[str, str]) -> NoticeEntry:
    """One entry from its field values, checked."""
    for field in ("Name", "Upstream", "Version", "License"):
        if not fields[field]:
            raise ValueError(f"{path}: {row_name}: the {field} is empty")
    shipped = fields["Shipped"]
    if shipped not in SHIPPED_VALUES:
        raise ValueError(f"{path}: {row_name}: Shipped is '{shipped}', not one of {', '.join(SHIPPED_VALUES)}")
    license_files: list[Path] = []
    for relative in _list_value(fields["License files"]):
        file = (path.parent / relative).resolve()
        if not file.is_file():
            raise ValueError(f"{path}: {row_name}: the license file '{relative}' does not exist")
        license_files.append(file)
    return NoticeEntry(name=fields["Name"], upstream=fields["Upstream"], version=fields["Version"],
                       license=fields["License"], shipped=shipped,
                       projects=tuple(_list_value(fields.get("Projects", "-"))),
                       license_files=tuple(license_files), source=path)


def read_notice_entries(path: Path) -> list[NoticeEntry]:
    """The entries of the "## Notices" section of `path` (a VENDOR.md: one entry; Resources/LICENSES.md: one per row).
    Raises ValueError naming the file and the row for a missing section, a missing or unknown row or column, a Shipped
    value other than yes, no or macOS, or a license file that does not exist."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise ValueError(f"{path}: cannot be read ({error})") from None
    header, data = _read_table(path, _section_lines(path, text))
    if [cell.lower() for cell in header] == ["field", "value"]:
        fields: dict[str, str] = {}
        for cells in data:
            field, value = _unquote(cells[0]), _unquote(cells[1])
            if field not in VENDOR_FIELDS:
                raise ValueError(f"{path}: unknown row '{field}' in the '## {NOTICES_SECTION}' table (expected "
                                 f"{', '.join(VENDOR_FIELDS)})")
            if field in fields:
                raise ValueError(f"{path}: the row '{field}' appears twice in the '## {NOTICES_SECTION}' table")
            fields[field] = value
        missing = [field for field in VENDOR_FIELDS if field not in fields]
        if missing:
            raise ValueError(f"{path}: the '## {NOTICES_SECTION}' table lacks the row(s) {', '.join(missing)}")
        return [_make_entry(path, f"the '{fields['Name'] or path.parent.name}' entry", fields)]

    columns = [_unquote(cell) for cell in header]
    unknown = [column for column in columns if column not in RESOURCE_COLUMNS]
    missing = [column for column in RESOURCE_COLUMNS if column not in columns]
    if unknown or missing:
        raise ValueError(f"{path}: the '## {NOTICES_SECTION}' table's columns must be {', '.join(RESOURCE_COLUMNS)} "
                         f"(unknown: {', '.join(unknown) or 'none'}; missing: {', '.join(missing) or 'none'})")
    if not data:
        raise ValueError(f"{path}: the '## {NOTICES_SECTION}' table has no rows")
    entries = []
    for number, cells in enumerate(data, start=1):
        fields = {column: _unquote(cell) for column, cell in zip(columns, cells)}
        entries.append(_make_entry(path, f"row {number} ('{fields['Name']}')", fields))
    return entries


def collect_notice_entries(repository_root: Path) -> list[NoticeEntry]:
    """Every entry of Vendor/*/VENDOR.md (the premake tool's directory has none and is skipped) and of
    Resources/LICENSES.md, sorted as the file orders them. Raises ValueError as read_notice_entries."""
    vendor = repository_root / "Vendor"
    libraries = sorted((directory for directory in vendor.iterdir() if directory.is_dir()),
                       key=lambda directory: (directory.name.casefold(), directory.name))
    entries: list[NoticeEntry] = []
    for library in libraries:
        manifest = library / "VENDOR.md"
        if library.name == PREMAKE_DIRECTORY and not manifest.is_file():
            continue
        if not manifest.is_file():
            raise ValueError(f"{library}: a vendored library without a VENDOR.md")
        entries += read_notice_entries(manifest)
    entries += read_notice_entries(repository_root / "Resources" / "LICENSES.md")
    return entries


def referenced_vendor_libraries(project_files: list[Path]) -> set[str]:
    """The names of the Vendor/<Lib> directories that the generated project files `project_files` reference anywhere
    (any path holding a Vendor/<Lib>/ segment, with forward or backward slashes, or ending in Vendor/<Lib>), the
    premake tool's directory excluded. Raises OSError for a file that does not read."""
    referenced: set[str] = set()
    for project_file in project_files:
        text = project_file.read_text(encoding="utf-8", errors="replace")
        for match in VENDOR_REFERENCE_PATTERN.finditer(text):
            name = match.group(1)
            if name != PREMAKE_DIRECTORY:
                referenced.add(name)
    return referenced


def _license_text(path: Path) -> str:
    """A license file verbatim, with LF line endings and without a byte order mark or trailing blank lines."""
    text = path.read_text(encoding="utf-8-sig")
    return text.replace("\r\n", "\n").replace("\r", "\n").rstrip()


def render_notices(entries: list[NoticeEntry], system: str) -> str:
    """The notices text of the entries among `entries` that exports on `system` ("windows", "linux" or "macosx", as
    Lib/paths.py names hosts) ship (see the module docstring)."""
    if system not in ("windows", "linux", "macosx"):
        raise ValueError(f"unknown system '{system}' (windows, linux or macosx)")
    shipped = [entry for entry in entries
               if entry.shipped == "yes" or (entry.shipped == "macOS" and system == "macosx")]
    shipped.sort(key=lambda entry: (entry.name.casefold(), entry.name))
    blocks = [INTRODUCTION]
    for entry in shipped:
        texts = [_license_text(file) for file in entry.license_files] or [NO_NOTICE_TEXT]
        blocks.append("\n".join([
            "=" * RULE_WIDTH,
            f"{entry.name} {entry.version}",
            f"License: {entry.license}",
            f"Source: {entry.upstream}",
            "-" * RULE_WIDTH,
            "\n\n".join(texts),
        ]))
    return "\n\n".join(blocks).rstrip("\n") + "\n"


def generate_notices(repository_root: Path, system: str) -> str:
    """render_notices(collect_notice_entries(repository_root), system)."""
    return render_notices(collect_notice_entries(repository_root), system)
