#!/usr/bin/env python3
"""Generate the heightmap fixtures in Tests/Data/Assets/Heightmaps (Roadmap M26, Docs/Decisions/0037-m26-contract.md
decision 20).

The PNG is built from the PNG specification with the standard library only: its image data is zlib with stored deflate
blocks, so no zlib build changes its bytes. Nothing is downloaded and nothing is random.

The fixtures (paths relative to Tests/Data/Assets/Heightmaps):
  Ramp16.png  17 x 17 16-bit greyscale PNG (colour type 0, bit depth 16, no interlace): sample (x, z), image column x
              and row z (row 0 the top row, the heightfield's row 0), is x * 4000 + z, at most 64,016, so every sample
              fits 16 bits and no two columns or rows share a value. "HeightfieldImporter: a 16-bit PNG and a RAW
              heightmap convert to the same heightfield bit for bit" converts it beside the same ramp as RAW bytes.

Run from the repository root: `python Tests/Data/Generate/MakeHeightmapFixtures.py` writes the files; `--check`
compares a fresh generation with the committed files and writes nothing; `--json` prints a machine-readable report on
stdout.

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

OUTPUT_DIRECTORY = Path(__file__).resolve().parents[1] / "Assets" / "Heightmaps"

RAMP_SIDE = 17
RAMP_COLUMN_STEP = 4000

# ---------------------------------------------------------------------------------------------------------------------
# PNG (stored deflate blocks)
# ---------------------------------------------------------------------------------------------------------------------


def adler32(data: bytes) -> int:
    first, second = 1, 0
    for byte in data:
        first = (first + byte) % 65521
        second = (second + first) % 65521
    return (second << 16) | first


def zlib_stored(data: bytes) -> bytes:
    """`data` as a zlib stream of stored (uncompressed) deflate blocks of at most 65,535 bytes each."""
    out = bytearray(b"\x78\x01")
    blocks = [data[index:index + 65535] for index in range(0, len(data), 65535)] or [b""]
    for index, block in enumerate(blocks):
        out.append(1 if index == len(blocks) - 1 else 0)
        out += struct.pack("<HH", len(block), len(block) ^ 0xFFFF)
        out += block
    out += struct.pack(">I", adler32(data))
    return bytes(out)


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", binascii.crc32(kind + payload) & 0xFFFFFFFF))


def grey16_png(side: int, sample: Callable[[int, int], int]) -> bytes:
    """A `side` x `side` 16-bit greyscale PNG whose pixel at column x and row z is sample(x, z), big-endian as PNG
    stores 16-bit samples, each row with filter type 0 (None)."""
    rows = bytearray()
    for z in range(side):
        rows.append(0)
        for x in range(side):
            value = sample(x, z)
            if not 0 <= value <= 0xFFFF:
                raise ValueError(f"sample ({x}, {z}) = {value} does not fit 16 bits")
            rows += struct.pack(">H", value)
    header = struct.pack(">IIBBBBB", side, side, 16, 0, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib_stored(bytes(rows)))
            + png_chunk(b"IEND", b""))


def make_ramp16() -> bytes:
    return grey16_png(RAMP_SIDE, lambda x, z: x * RAMP_COLUMN_STEP + z)


# ---------------------------------------------------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------------------------------------------------

FIXTURES: dict[str, Callable[[], bytes]] = {
    "Ramp16.png": make_ramp16,
}


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the heightmap fixtures in Tests/Data/Assets/Heightmaps.",
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
    status = "FAILED" if failed else "passed"
    print(f"{status}: {len(FIXTURES)} heightmap fixtures, {len(differences)} {verb}", file=out)
    if options.json:
        document = {"status": "failed" if failed else "passed", "check": options.check, "fixtures": sorted(FIXTURES),
                    "differences": differences, "errors": errors}
        print(json.dumps(document, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
