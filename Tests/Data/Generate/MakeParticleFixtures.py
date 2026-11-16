#!/usr/bin/env python3
"""Generate the particle flipbook fixture in Tests/Data/Assets/Particles (Roadmap M33;
Docs/Decisions/0048-m33-particle-look.md decision 8).

The image is built from the PNG specification with the standard library only: stored (uncompressed) deflate
blocks with an Adler-32 checksum, so no zlib build can change its bytes. Every texel is a function of its frame
and its position computed in integer arithmetic, so the output is byte-for-byte identical on every platform and
Python 3.10+. Nothing is downloaded and nothing is random.

The fixtures (paths relative to Tests/Data/Assets/Particles):
  Flame.png  128 x 128, 8-bit RGBA: a 4 x 4 grid of 32 x 32 flame frames, frame f in row f // 4, column f % 4,
             each a teardrop of fire on a transparent ground, its base at the frame's row 28 and its tip swaying
             and flickering with f (FLAME_SWAY and FLAME_HEIGHT): a white-yellow core turning orange and red toward
             its edges and its tip, its alpha soft at the edges and fading toward the tip

FeatureTest's scaffolds import a copy (Projects/FeatureTest/Scaffold/Sources/Particles/Flame.png), as M26's LUT
and heightmap fixtures.

Run from the repository root: `python Tests/Data/Generate/MakeParticleFixtures.py` writes the files; `--check`
compares a fresh generation with the committed files and writes nothing; `--json` prints a machine-readable
report on stdout.

Exit codes: 0 success, 1 --check found a difference or a file could not be written, 2 usage error.
"""

from __future__ import annotations

import argparse
import binascii
import json
import struct
import sys
from collections.abc import Callable
from pathlib import Path

OUTPUT_DIRECTORY = Path(__file__).resolve().parents[1] / "Assets" / "Particles"

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
STORED_BLOCK_LIMIT = 65535

FRAME_SIZE = 32
FRAME_COLUMNS = 4
FRAME_COUNT = 16
FLAME_BASE = 28
# Per frame: the tip's sideways sway in texels and the flame's height above its base in texels.
FLAME_SWAY = (0, 1, 2, 3, 3, 2, 1, 0, 0, -1, -2, -3, -3, -2, -1, 0)
FLAME_HEIGHT = (22, 23, 24, 23, 22, 21, 20, 21, 22, 23, 24, 23, 22, 21, 20, 21)
FLAME_HALF_WIDTH = 9


def adler32(data: bytes) -> int:
    first, second = 1, 0
    for byte in data:
        first = (first + byte) % 65521
        second = (second + first) % 65521
    return (second << 16) | first


def zlib_stored(data: bytes) -> bytes:
    """A zlib stream of stored deflate blocks (RFC 1950, RFC 1951 section 3.2.4)."""
    out = bytearray(b"\x78\x01")
    blocks = [data[index:index + STORED_BLOCK_LIMIT] for index in range(0, len(data), STORED_BLOCK_LIMIT)] or [b""]
    for index, block in enumerate(blocks):
        out.append(1 if index == len(blocks) - 1 else 0)
        out += struct.pack("<HH", len(block), len(block) ^ 0xFFFF)
        out += block
    out += struct.pack(">I", adler32(data))
    return bytes(out)


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = binascii.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)


def png_rgba(width: int, height: int, rows: list[bytes]) -> bytes:
    """An 8-bit RGBA PNG of packed scanlines (filter type 0 on every row)."""
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    raw = b"".join(b"\x00" + row for row in rows)
    return PNG_SIGNATURE + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib_stored(raw)) + png_chunk(b"IEND", b"")


def clamp(value: int, low: int, high: int) -> int:
    return max(low, min(high, value))


def flame_texel(frame: int, x: int, y: int) -> tuple[int, int, int, int]:
    """Texel (x, y) of flame frame `frame` as (r, g, b, a), all in integer arithmetic in 1/256 texel units."""
    height = FLAME_HEIGHT[frame]
    above = FLAME_BASE - y
    if above < -3 or above > height:
        return (0, 0, 0, 0)
    if above >= 0:
        rise = above * 256 // height  # 0 at the base, 256 at the tip
        half_width = FLAME_HALF_WIDTH * (256 - rise) * (256 + rise) // 256  # rounded, tapering to the tip
    else:
        rise = 0
        half_width = FLAME_HALF_WIDTH * 256 * (3 + above) // 3  # the rounded bottom below the base
    centre = (FRAME_SIZE // 2) * 256 + FLAME_SWAY[frame] * rise
    distance = abs(x * 256 + 128 - centre)
    if half_width <= 0 or distance >= half_width:
        return (0, 0, 0, 0)
    edge = clamp((half_width - distance) * 255 // (3 * 256), 0, 255)
    heat = (255 - distance * 255 // half_width) * (256 - rise // 2) // 256
    red = 255
    green = clamp(70 + heat * 185 // 255, 0, 255)
    blue = clamp(heat * heat // 255 * 210 // 255, 0, 255)
    alpha = edge * (256 - rise // 3) // 256
    return (red, green, blue, alpha)


def make_flame() -> bytes:
    """The Flame.png fixture (see the module docstring)."""
    size = FRAME_SIZE * FRAME_COLUMNS
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            frame = (y // FRAME_SIZE) * FRAME_COLUMNS + x // FRAME_SIZE
            row += bytes(flame_texel(frame, x % FRAME_SIZE, y % FRAME_SIZE))
        rows.append(bytes(row))
    return png_rgba(size, size, rows)


FIXTURES: dict[str, Callable[[], bytes]] = {
    "Flame.png": make_flame,
}


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the particle flipbook fixture in Tests/Data/Assets/Particles.",
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
    print(f"{'FAILED' if failed else 'passed'}: {len(FIXTURES)} particle fixtures, {len(differences)} {verb}", file=out)
    if options.json:
        document = {"status": "failed" if failed else "passed", "check": options.check, "fixtures": sorted(FIXTURES),
                    "differences": differences, "errors": errors}
        print(json.dumps(document, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
