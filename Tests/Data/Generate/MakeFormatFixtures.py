#!/usr/bin/env python3
"""Generate the first-party format fixtures that no other generator writes: a Radiance HDR environment and a TrueType
font in Tests/Data/Assets (Roadmap M14, Docs/Decisions/0022-m14-contract.md decision 16).

Both files are built from their format's specification with the standard library only. Every value is a function of its
position, computed with exact integer arithmetic or IEEE double operations in a fixed order (no transcendental
function), so the bytes are identical on every platform and Python 3.10+. Nothing is downloaded and nothing is random.

The fixtures (paths relative to Tests/Data/Assets):
  Environments/Gradient.hdr  32 x 16 equirectangular sky gradient (2:1, as the environment importer requires) with one
                             bright sun texel, in flat (not run-length encoded) RGBE scanlines
  Fonts/Glyphs.ttf           a TrueType font with quadratic outlines: .notdef (a box with a counter), space (no
                             outline), A and V (straight contours, A with a counter) and O (quadratic curves, two
                             contours); a format 4 Unicode cmap, the head, hhea, maxp, hmtx, loca, glyf, name and post
                             tables, and a legacy kern table (format 0) that kerns A against V in both orders

The OTF, MP3 and Ogg Vorbis fixtures are third-party files pinned by Scripts/FetchAssets.py fixture; the WAV and FLAC
fixtures come from MakeAudioFixtures.py, the textures (including Baseline.jpeg) from MakeTextureFixtures.py and the glTF
fixtures from MakeGltfFixtures.py.

Run from the repository root: `python Tests/Data/Generate/MakeFormatFixtures.py` writes the files; `--check` compares a
fresh generation with the committed files and writes nothing; `--json` prints a machine-readable report on stdout.

Exit codes: 0 success, 1 --check found a difference or a file could not be written, 2 usage error.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from collections.abc import Callable
from pathlib import Path

OUTPUT_DIRECTORY = Path(__file__).resolve().parents[1] / "Assets"

# ---------------------------------------------------------------------------------------------------------------------
# Radiance HDR (RGBE)
# ---------------------------------------------------------------------------------------------------------------------

HDR_WIDTH = 32
HDR_HEIGHT = 16
HDR_SUN = (8, 4)  # (column, row) of the bright texel
HDR_SUN_RADIANCE = (24.0, 20.0, 14.0)


def sky_radiance(column: int, row: int) -> tuple[float, float, float]:
    """Linear RGB radiance of a texel: a blue zenith fading to a pale horizon above, a brown ground below, with a gentle
    variation around the horizon (the longitude) so the image has detail in both directions."""
    if (column, row) == HDR_SUN:
        return HDR_SUN_RADIANCE
    longitude = 1.0 + 0.0625 * (column % 8) / 8.0
    if row < HDR_HEIGHT // 2:
        t = (row + 0.5) / (HDR_HEIGHT / 2)  # 0 at the zenith, 1 at the horizon
        return ((0.25 + 0.75 * t) * longitude, (0.45 + 0.55 * t) * longitude, (1.0 + 0.25 * t) * longitude)
    t = (row - HDR_HEIGHT // 2 + 0.5) / (HDR_HEIGHT / 2)  # 0 at the horizon, 1 at the nadir
    return ((0.30 - 0.15 * t) * longitude, (0.22 - 0.12 * t) * longitude, (0.12 - 0.07 * t) * longitude)


def rgbe(red: float, green: float, blue: float) -> bytes:
    """Ward's shared-exponent encoding: the mantissas of the largest channel's binary exponent, truncated."""
    largest = max(red, green, blue)
    if largest < 1e-32:
        return bytes(4)
    mantissa, exponent = math.frexp(largest)  # largest = mantissa * 2^exponent, mantissa in [0.5, 1)
    scale = mantissa * 256.0 / largest
    return bytes([int(red * scale), int(green * scale), int(blue * scale), exponent + 128])


def make_gradient_hdr() -> bytes:
    header = (b"#?RADIANCE\n# Tests/Data/Generate/MakeFormatFixtures.py\nFORMAT=32-bit_rle_rgbe\n\n"
              + f"-Y {HDR_HEIGHT} +X {HDR_WIDTH}\n".encode("ascii"))
    scanlines = bytearray()
    for row in range(HDR_HEIGHT):
        line = b"".join(rgbe(*sky_radiance(column, row)) for column in range(HDR_WIDTH))
        # A scanline that starts with 2, 2 and a byte below 128 would read as a run-length encoded one; a normalized
        # pixel's largest mantissa is at least 128, so a flat scanline never does.
        if line[0] == 2 and line[1] == 2 and line[2] < 128:
            raise ValueError(f"scanline {row} would decode as run-length encoded")
        scanlines += line
    return header + bytes(scanlines)


# ---------------------------------------------------------------------------------------------------------------------
# TrueType (sfnt)
# ---------------------------------------------------------------------------------------------------------------------

UNITS_PER_EM = 1000
ASCENT = 800
DESCENT = -200
ON = True
OFF = False

Point = tuple[int, int, bool]  # x, y, on-curve


class Glyph:
    """A simple glyph: its contours (outer contours clockwise, counters counter-clockwise, y up) and advance width."""

    def __init__(self, name: str, advance: int, contours: list[list[Point]]) -> None:
        self.name = name
        self.advance = advance
        self.contours = contours

    @property
    def points(self) -> list[Point]:
        return [point for contour in self.contours for point in contour]

    @property
    def bounds(self) -> tuple[int, int, int, int]:
        """(xMin, yMin, xMax, yMax) over every point; zeros for a glyph without an outline."""
        points = self.points
        if not points:
            return (0, 0, 0, 0)
        return (min(p[0] for p in points), min(p[1] for p in points), max(p[0] for p in points),
                max(p[1] for p in points))

    def encode(self) -> bytes:
        """The glyf description (no instructions, one flag byte and two int16 deltas per point), padded to an even
        length for the short loca format."""
        if not self.contours:
            return b""
        x_min, y_min, x_max, y_max = self.bounds
        out = bytearray(struct.pack(">hhhhh", len(self.contours), x_min, y_min, x_max, y_max))
        end = -1
        for contour in self.contours:
            end += len(contour)
            out += struct.pack(">H", end)
        out += struct.pack(">H", 0)  # instructionLength
        out += bytes(0x01 if on else 0x00 for _, _, on in self.points)
        previous_x = previous_y = 0
        xs = bytearray()
        ys = bytearray()
        for x, y, _ in self.points:
            xs += struct.pack(">h", x - previous_x)
            ys += struct.pack(">h", y - previous_y)
            previous_x, previous_y = x, y
        out += xs + ys
        if len(out) % 2:
            out.append(0)
        return bytes(out)


GLYPHS = [
    Glyph(".notdef", 500, [
        [(50, 0, ON), (50, 700, ON), (450, 700, ON), (450, 0, ON)],
        [(100, 50, ON), (400, 50, ON), (400, 650, ON), (100, 650, ON)],
    ]),
    Glyph("space", 250, []),
    Glyph("A", 600, [
        [(20, 0, ON), (300, 700, ON), (580, 0, ON)],
        [(200, 150, ON), (400, 150, ON), (300, 400, ON)],
    ]),
    Glyph("O", 600, [
        [(300, 0, ON), (50, 0, OFF), (50, 350, ON), (50, 700, OFF), (300, 700, ON), (550, 700, OFF), (550, 350, ON),
         (550, 0, OFF)],
        [(300, 150, ON), (450, 150, OFF), (450, 350, ON), (450, 550, OFF), (300, 550, ON), (150, 550, OFF),
         (150, 350, ON), (150, 150, OFF)],
    ]),
    Glyph("V", 600, [
        [(20, 700, ON), (140, 700, ON), (300, 250, ON), (460, 700, ON), (580, 700, ON), (300, 0, ON)],
    ]),
]
CHARACTER_MAP = {0x20: 1, 0x41: 2, 0x4F: 3, 0x56: 4}  # codepoint -> glyph index
KERNING = {(2, 4): -80, (4, 2): -60}  # (left glyph, right glyph) -> adjustment in font units
NAMES = {
    0: "First-party test font of the Engine repository, generated by Tests/Data/Generate/MakeFormatFixtures.py",
    1: "Glyphs",
    2: "Regular",
    3: "Glyphs Regular 1.000",
    4: "Glyphs Regular",
    5: "Version 1.000",
    6: "Glyphs-Regular",
}


def signed_area(contour: list[Point]) -> int:
    """Twice the signed area of a contour's control polygon: positive counter-clockwise (y up)."""
    return sum(contour[index][0] * contour[(index + 1) % len(contour)][1]
               - contour[(index + 1) % len(contour)][0] * contour[index][1] for index in range(len(contour)))


def check_orientation() -> None:
    """TrueType fills by the non-zero rule with outer contours clockwise: the first contour of each glyph is an outer
    one and every further contour is a counter."""
    for glyph in GLYPHS:
        for index, contour in enumerate(glyph.contours):
            if (signed_area(contour) < 0) != (index == 0):
                raise ValueError(f"contour {index} of '{glyph.name}' has the wrong orientation")


def search_parameters(count: int, unit: int) -> tuple[int, int, int]:
    """(searchRange, entrySelector, rangeShift) of a binary-searchable array of `count` entries of `unit` bytes."""
    power = 1
    selector = 0
    while power * 2 <= count:
        power *= 2
        selector += 1
    return power * unit, selector, count * unit - power * unit


def table_checksum(data: bytes) -> int:
    padded = data + bytes(-len(data) % 4)
    return sum(struct.unpack(f">{len(padded) // 4}I", padded)) & 0xFFFFFFFF


def make_cmap() -> bytes:
    """Format 4 with one segment per mapped codepoint (idDelta maps it, no glyph id array) and the final 0xFFFF one,
    listed for Unicode (platform 0, encoding 3) and Windows Unicode BMP (platform 3, encoding 1)."""
    segments = [(code, code, (glyph - code) & 0xFFFF) for code, glyph in sorted(CHARACTER_MAP.items())]
    segments.append((0xFFFF, 0xFFFF, 1))
    count = len(segments)
    search_range, entry_selector, range_shift = search_parameters(count, 2)
    body = struct.pack(">HHH", count * 2, search_range, entry_selector) + struct.pack(">H", range_shift)
    body += b"".join(struct.pack(">H", end) for _, end, _ in segments)
    body += struct.pack(">H", 0)  # reservedPad
    body += b"".join(struct.pack(">H", start) for start, _, _ in segments)
    body += b"".join(struct.pack(">H", delta) for _, _, delta in segments)
    body += b"".join(struct.pack(">H", 0) for _ in segments)  # idRangeOffset
    subtable = struct.pack(">HHH", 4, 6 + len(body), 0) + body
    header = struct.pack(">HH", 0, 2) + struct.pack(">HHI", 0, 3, 20) + struct.pack(">HHI", 3, 1, 20)
    return header + subtable


def make_glyf_and_loca() -> tuple[bytes, bytes]:
    glyf = bytearray()
    offsets = []
    for glyph in GLYPHS:
        offsets.append(len(glyf))
        glyf += glyph.encode()
    offsets.append(len(glyf))
    loca = b"".join(struct.pack(">H", offset // 2) for offset in offsets)
    return bytes(glyf), loca


def font_bounds() -> tuple[int, int, int, int]:
    outlined = [glyph.bounds for glyph in GLYPHS if glyph.contours]
    return (min(b[0] for b in outlined), min(b[1] for b in outlined), max(b[2] for b in outlined),
            max(b[3] for b in outlined))


def make_head() -> bytes:
    x_min, y_min, x_max, y_max = font_bounds()
    return struct.pack(">IIIIHHqqhhhhHHhhh", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0x0003, UNITS_PER_EM, 0, 0, x_min,
                       y_min, x_max, y_max, 0, 8, 2, 0, 0)


def make_hhea() -> bytes:
    outlined = [glyph for glyph in GLYPHS if glyph.contours]
    min_left = min(glyph.bounds[0] for glyph in outlined)
    min_right = min(glyph.advance - glyph.bounds[2] for glyph in outlined)
    max_extent = max(glyph.bounds[2] for glyph in outlined)
    return struct.pack(">IhhhHhhhhhhhhhhhH", 0x00010000, ASCENT, DESCENT, 0, max(glyph.advance for glyph in GLYPHS),
                       min_left, min_right, max_extent, 1, 0, 0, 0, 0, 0, 0, 0, len(GLYPHS))


def make_maxp() -> bytes:
    points = max(len(glyph.points) for glyph in GLYPHS)
    contours = max(len(glyph.contours) for glyph in GLYPHS)
    return struct.pack(">IHHHHHHHHHHHHHH", 0x00010000, len(GLYPHS), points, contours, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0)


def make_hmtx() -> bytes:
    return b"".join(struct.pack(">Hh", glyph.advance, glyph.bounds[0]) for glyph in GLYPHS)


def make_kern() -> bytes:
    """Version 0 with one horizontal format 0 subtable; pairs sorted by (left, right)."""
    pairs = sorted(KERNING.items())
    search_range, entry_selector, range_shift = search_parameters(len(pairs), 6)
    body = struct.pack(">HHHH", len(pairs), search_range, entry_selector, range_shift)
    body += b"".join(struct.pack(">HHh", left, right, value) for (left, right), value in pairs)
    subtable = struct.pack(">HHH", 0, 6 + len(body), 0x0001) + body
    return struct.pack(">HH", 0, 1) + subtable


def make_name() -> bytes:
    strings = bytearray()
    records = bytearray()
    for name_id, text in sorted(NAMES.items()):
        encoded = text.encode("utf-16-be")
        records += struct.pack(">HHHHHH", 3, 1, 0x0409, name_id, len(encoded), len(strings))
        strings += encoded
    return struct.pack(">HHH", 0, len(NAMES), 6 + len(records)) + bytes(records) + bytes(strings)


def make_post() -> bytes:
    return struct.pack(">IIhhIIIII", 0x00030000, 0, -100, 50, 0, 0, 0, 0, 0)


def make_glyphs_ttf() -> bytes:
    check_orientation()
    glyf, loca = make_glyf_and_loca()
    tables = {
        "cmap": make_cmap(),
        "glyf": glyf,
        "head": make_head(),
        "hhea": make_hhea(),
        "hmtx": make_hmtx(),
        "kern": make_kern(),
        "loca": loca,
        "maxp": make_maxp(),
        "name": make_name(),
        "post": make_post(),
    }
    tags = sorted(tables)
    search_range, entry_selector, range_shift = search_parameters(len(tags), 16)
    directory = bytearray(struct.pack(">IHHHH", 0x00010000, len(tags), search_range, entry_selector, range_shift))
    body = bytearray()
    offset = 12 + 16 * len(tags)
    head_offset = 0
    for tag in tags:
        data = tables[tag]
        if tag == "head":
            head_offset = offset + len(body)
        directory += tag.encode("ascii") + struct.pack(">III", table_checksum(data), offset + len(body), len(data))
        body += data + bytes(-len(data) % 4)
    font = bytearray(directory + body)
    adjustment = (0xB1B0AFBA - table_checksum(bytes(font))) & 0xFFFFFFFF
    font[head_offset + 8:head_offset + 12] = struct.pack(">I", adjustment)
    return bytes(font)


# ---------------------------------------------------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------------------------------------------------

FIXTURES: dict[str, Callable[[], bytes]] = {
    "Environments/Gradient.hdr": make_gradient_hdr,
    "Fonts/Glyphs.ttf": make_glyphs_ttf,
}


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the Radiance HDR and TrueType format fixtures in Tests/Data/Assets.",
        epilog="Exit codes: 0 success, 1 --check found a difference or a file could not be written, 2 usage error.",
    )
    parser.add_argument("--check", action="store_true", help="compare with the committed files and write nothing")
    parser.add_argument("--json", action="store_true", help="print a machine-readable report on stdout")
    return parser.parse_args(arguments)


def generate(output: Path, check: bool) -> tuple[list[str], list[str]]:
    """Writes (or, with `check`, compares) every fixture under `output`; returns the files that differed and the
    errors."""
    differences: list[str] = []
    errors: list[str] = []
    for name, make in FIXTURES.items():
        content = make()
        path = output / name
        current = path.read_bytes() if path.is_file() else None
        if current == content:
            continue
        differences.append(name)
        if check:
            continue
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        except OSError as error:
            errors.append(f"{name}: {error}")
    return differences, errors


def main(arguments: list[str] | None = None) -> int:
    options = parse_arguments(arguments)
    differences, errors = generate(OUTPUT_DIRECTORY, options.check)
    failed = bool(errors) or (options.check and bool(differences))
    out = sys.stderr if options.json else sys.stdout
    verb = "differ from a fresh generation" if options.check else "written"
    for name in differences:
        print(f"  {name}: {verb}", file=out)
    for error in errors:
        print(f"  error: {error}", file=out)
    print(f"{'FAILED' if failed else 'passed'}: {len(FIXTURES)} format fixtures, {len(differences)} {verb}", file=out)
    if options.json:
        document = {"status": "failed" if failed else "passed", "check": options.check, "fixtures": sorted(FIXTURES),
                    "differences": differences, "errors": errors}
        print(json.dumps(document, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
