#!/usr/bin/env python3
"""Generate the texture importer fixtures in Tests/Data/Assets/Textures (Roadmap M6,
Docs/Decisions/0010-m6-decisions.md).

Every image is built from its file format's specification with the standard library only. PNG files hold stored
(uncompressed) deflate blocks with an Adler-32 checksum, so no zlib build can change their bytes; the JPEG encoder uses
the standard (Annex K) quantization and Huffman tables, a separable DCT whose cosines are literal constants, and only
IEEE double additions and multiplications in a fixed order, so the output is byte-for-byte identical on every platform
and Python 3.10+. Nothing is downloaded and nothing is random: every texel is a function of its position.

The fixtures (paths relative to Tests/Data/Assets/Textures), each exercising one decoding path of the texture importer:
  Rgba.png       16 x 8, 8-bit RGBA, alpha varying across the image
  Rgb.png        7 x 5, 8-bit RGB (odd sizes: every mip level rounds down)
  Grey.png       4 x 4, 8-bit greyscale (replicated to RGB, alpha 255)
  GreyAlpha.png  5 x 3, 8-bit greyscale with alpha
  Palette.png    6 x 6, 4-bit palette with a tRNS chunk (transparent entries)
  Deep.png       3 x 2, 16-bit RGB (reduced to the high byte)
  Normal.png     8 x 8, 8-bit RGB tangent-space normal map of a dome (for the NormalMap usage)
  Photo.jpg      16 x 16, baseline JPEG, YCbCr 4:4:4, quality 90
  GreyPhoto.jpg  8 x 8, baseline JPEG, one greyscale component
  Solid.bmp      5 x 3, 24-bit BMP (bottom-up rows padded to 4 bytes)
  Alpha.tga      3 x 2, uncompressed 32-bit TGA with a bottom-left origin
  Rle.tga        4 x 2, run-length encoded 24-bit TGA with a top-left origin
  Truncated.png  a PNG cut off inside its header chunk: fails to import, the same way every time

Run from the repository root: `python Tests/Data/Generate/MakeTextureFixtures.py` writes the files; `--check` compares a
fresh generation with the committed files and writes nothing; `--json` prints a machine-readable report on stdout.

Exit codes: 0 success, 1 --check found a difference or a file could not be written, 2 usage error.
"""

from __future__ import annotations

import argparse
import binascii
import json
import math
import struct
import sys
from collections.abc import Callable
from pathlib import Path

OUTPUT_DIRECTORY = Path(__file__).resolve().parents[1] / "Assets" / "Textures"

Pixel = tuple[int, ...]

# ---------------------------------------------------------------------------------------------------------------------
# PNG
# ---------------------------------------------------------------------------------------------------------------------

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
STORED_BLOCK_LIMIT = 65535


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
        final = 1 if index == len(blocks) - 1 else 0
        out.append(final)
        out += struct.pack("<HH", len(block), len(block) ^ 0xFFFF)
        out += block
    out += struct.pack(">I", adler32(data))
    return bytes(out)


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = binascii.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)


def png(width: int, height: int, bit_depth: int, color_type: int, rows: list[bytes],
        extra: list[bytes] | None = None) -> bytes:
    """A PNG of pre-packed scanlines (filter type 0 on every row)."""
    header = struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0)
    raw = b"".join(b"\x00" + row for row in rows)
    chunks = [png_chunk(b"IHDR", header)] + (extra or [])
    chunks += [png_chunk(b"IDAT", zlib_stored(raw)), png_chunk(b"IEND", b"")]
    return PNG_SIGNATURE + b"".join(chunks)


def pack_rows(width: int, height: int, texel: Callable[[int, int], Pixel], bytes_per_channel: int = 1) -> list[bytes]:
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            for channel in texel(x, y):
                row += channel.to_bytes(bytes_per_channel, "big")
        rows.append(bytes(row))
    return rows


def make_rgba() -> bytes:
    def texel(x: int, y: int) -> Pixel:
        return (x * 16 % 256, y * 32 % 256, (x ^ y) * 17 % 256, 255 - x * 15)
    return png(16, 8, 8, 6, pack_rows(16, 8, texel))


def make_rgb() -> bytes:
    def texel(x: int, y: int) -> Pixel:
        return ((x * 37 + 11) % 256, (y * 59 + 23) % 256, (x * y * 13 + 5) % 256)
    return png(7, 5, 8, 2, pack_rows(7, 5, texel))


def make_grey() -> bytes:
    return png(4, 4, 8, 0, pack_rows(4, 4, lambda x, y: ((x + y * 4) * 17,)))


def make_grey_alpha() -> bytes:
    return png(5, 3, 8, 4, pack_rows(5, 3, lambda x, y: (x * 60, 255 - y * 100)))


def make_palette() -> bytes:
    palette = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0])
    transparency = bytes([255, 128, 0])  # entry 3 stays opaque (tRNS may be shorter than PLTE)
    rows = []
    for y in range(6):
        indices = [(x + y) % 4 for x in range(6)]
        rows.append(bytes((indices[index] << 4) | indices[index + 1] for index in range(0, 6, 2)))
    return png(6, 6, 4, 3, rows, [png_chunk(b"PLTE", palette), png_chunk(b"tRNS", transparency)])


def make_deep() -> bytes:
    def texel(x: int, y: int) -> Pixel:
        return (x * 30000 + 1234, y * 50000 + 4321, 65535 - x * y * 9000)
    return png(3, 2, 16, 2, pack_rows(3, 2, texel, 2))


def make_normal() -> bytes:
    """A dome: normals tilt outwards from the centre, encoded as value = (n + 1) / 2 * 255."""
    def texel(x: int, y: int) -> Pixel:
        dx = (x + 0.5 - 4.0) / 4.0
        dy = (y + 0.5 - 4.0) / 4.0
        dz = math.sqrt(max(0.0, 1.0 - dx * dx - dy * dy))
        length = math.sqrt(dx * dx + dy * dy + dz * dz)
        return tuple(math.floor((component / length * 0.5 + 0.5) * 255.0 + 0.5) for component in (dx, dy, dz))
    return png(8, 8, 8, 2, pack_rows(8, 8, texel))


def make_truncated() -> bytes:
    return make_rgb()[:20]


# ---------------------------------------------------------------------------------------------------------------------
# Baseline JPEG (ITU-T T.81)
# ---------------------------------------------------------------------------------------------------------------------

ZIGZAG = [
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
]
LUMA_QUANTIZATION = [
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
]
CHROMA_QUANTIZATION = [
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
]
DC_LUMA_BITS = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
DC_LUMA_VALUES = list(range(12))
DC_CHROMA_BITS = [0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0]
DC_CHROMA_VALUES = list(range(12))
AC_LUMA_BITS = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D]
AC_LUMA_VALUES = bytes.fromhex(
    "01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738393a"
    "434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8"
    "a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa"
)
AC_CHROMA_BITS = [0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77]
AC_CHROMA_VALUES = bytes.fromhex(
    "000102031104052131061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a262728292a35363738"
    "393a434445464748494a535455565758595a636465666768696a737475767778797a82838485868788898a92939495969798999aa2a3a4a5"
    "a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9fa"
)
JPEG_QUALITY = 90
# cos(k * pi / 16) for k = 0..8, as literals: the DCT never calls the C runtime's cosine.
COSINES = [
    1.0, 0.9807852804032304, 0.9238795325112867, 0.8314696123025452, 0.7071067811865476, 0.5555702330196022,
    0.3826834323650898, 0.19509032201612825, 0.0,
]


def cosine(numerator: int) -> float:
    """cos(numerator * pi / 16) from the literal table, by symmetry."""
    angle = numerator % 32
    if angle > 16:
        angle = 32 - angle
    return COSINES[angle] if angle <= 8 else -COSINES[16 - angle]


DCT_BASIS = [[cosine((2 * x + 1) * u) for x in range(8)] for u in range(8)]


def scaled_quantization(table: list[int]) -> list[int]:
    scale = 200 - 2 * JPEG_QUALITY
    return [min(255, max(1, (value * scale + 50) // 100)) for value in table]


def zigzag(table: list[int]) -> bytes:
    """A table in natural order written in the zigzag order of a DQT segment."""
    return bytes(table[ZIGZAG[index]] for index in range(64))


def huffman_codes(bits: list[int], values: bytes | list[int]) -> dict[int, tuple[int, int]]:
    """The code (value -> (code, length)) of a Huffman table given as its BITS and HUFFVAL lists (T.81 Annex C)."""
    codes: dict[int, tuple[int, int]] = {}
    code = 0
    index = 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            codes[values[index]] = (code, length)
            code += 1
            index += 1
        code <<= 1
    return codes


class BitWriter:
    def __init__(self) -> None:
        self.data = bytearray()
        self.accumulator = 0
        self.count = 0

    def write(self, value: int, length: int) -> None:
        for shift in range(length - 1, -1, -1):
            self.accumulator = (self.accumulator << 1) | ((value >> shift) & 1)
            self.count += 1
            if self.count == 8:
                self.data.append(self.accumulator)
                if self.accumulator == 0xFF:
                    self.data.append(0)  # byte stuffing
                self.accumulator = 0
                self.count = 0

    def flush(self) -> bytes:
        if self.count:
            self.write((1 << (8 - self.count)) - 1, 8 - self.count)  # pad with one bits
        return bytes(self.data)


def magnitude_category(value: int) -> int:
    return abs(value).bit_length()


def magnitude_bits(value: int, category: int) -> int:
    return value if value >= 0 else value + (1 << category) - 1


def forward_dct(block: list[float]) -> list[float]:
    """The 8 x 8 forward DCT of level-shifted samples (row-major), separable, in a fixed operation order."""
    rows = [[sum(block[y * 8 + x] * DCT_BASIS[u][x] for x in range(8)) for u in range(8)] for y in range(8)]
    out = [0.0] * 64
    for v in range(8):
        for u in range(8):
            total = sum(rows[y][u] * DCT_BASIS[v][y] for y in range(8))
            scale = (0.7071067811865476 if u == 0 else 1.0) * (0.7071067811865476 if v == 0 else 1.0)
            out[v * 8 + u] = 0.25 * scale * total
    return out


def encode_block(writer: BitWriter, block: list[float], quantization: list[int], previous_dc: int,
                 dc_codes: dict[int, tuple[int, int]], ac_codes: dict[int, tuple[int, int]]) -> int:
    coefficients = forward_dct(block)
    # Zigzag order; the quantization tables are in natural (row-major) order like the coefficients.
    quantized = [math.floor(coefficients[ZIGZAG[index]] / quantization[ZIGZAG[index]] + 0.5) for index in range(64)]
    difference = quantized[0] - previous_dc
    category = magnitude_category(difference)
    writer.write(*dc_codes[category])
    writer.write(magnitude_bits(difference, category), category)
    run = 0
    for index in range(1, 64):
        value = quantized[index]
        if value == 0:
            run += 1
            continue
        while run > 15:
            writer.write(*ac_codes[0xF0])  # ZRL: sixteen zeros
            run -= 16
        category = magnitude_category(value)
        writer.write(*ac_codes[(run << 4) | category])
        writer.write(magnitude_bits(value, category), category)
        run = 0
    if run:
        writer.write(*ac_codes[0x00])  # EOB
    return quantized[0]


def jpeg_segment(marker: int, payload: bytes) -> bytes:
    return struct.pack(">HH", 0xFF00 | marker, len(payload) + 2) + payload


def jpeg(width: int, height: int, planes: list[list[float]]) -> bytes:
    """A baseline JPEG of one (greyscale) or three (YCbCr 4:4:4) planes of level-shifted samples; the sizes are
    multiples of 8."""
    luma = scaled_quantization(LUMA_QUANTIZATION)
    chroma = scaled_quantization(CHROMA_QUANTIZATION)
    color = len(planes) == 3
    out = bytearray(b"\xff\xd8")
    out += jpeg_segment(0xE0, b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")
    out += jpeg_segment(0xDB, bytes([0]) + zigzag(luma) + (bytes([1]) + zigzag(chroma) if color else b""))
    components = [(1, 0), (2, 1), (3, 1)][:len(planes)]
    frame = struct.pack(">BHHB", 8, height, width, len(components))
    frame += b"".join(bytes([identifier, 0x11, table]) for identifier, table in components)
    out += jpeg_segment(0xC0, frame)
    tables = [(0x00, DC_LUMA_BITS, DC_LUMA_VALUES), (0x10, AC_LUMA_BITS, AC_LUMA_VALUES)]
    if color:
        tables += [(0x01, DC_CHROMA_BITS, DC_CHROMA_VALUES), (0x11, AC_CHROMA_BITS, AC_CHROMA_VALUES)]
    for table_class, bits, values in tables:
        out += jpeg_segment(0xC4, bytes([table_class]) + bytes(bits) + bytes(values))
    selectors = b"".join(bytes([identifier, (table << 4) | table]) for identifier, table in components)
    scan = bytes([len(components)]) + selectors
    out += jpeg_segment(0xDA, scan + b"\x00\x3f\x00")

    codes = [(huffman_codes(DC_LUMA_BITS, DC_LUMA_VALUES), huffman_codes(AC_LUMA_BITS, AC_LUMA_VALUES))]
    codes.append((huffman_codes(DC_CHROMA_BITS, DC_CHROMA_VALUES), huffman_codes(AC_CHROMA_BITS, AC_CHROMA_VALUES)))
    writer = BitWriter()
    previous = [0] * len(planes)
    for block_y in range(0, height, 8):
        for block_x in range(0, width, 8):
            for index, plane in enumerate(planes):
                block = [plane[(block_y + y) * width + block_x + x] for y in range(8) for x in range(8)]
                table = components[index][1]
                quantization = luma if table == 0 else chroma
                previous[index] = encode_block(writer, block, quantization, previous[index], codes[table][0],
                                               codes[table][1])
    out += writer.flush()
    out += b"\xff\xd9"
    return bytes(out)


def make_photo() -> bytes:
    width = height = 16
    planes: list[list[float]] = [[], [], []]
    for y in range(height):
        for x in range(width):
            red, green, blue = (x * 15, y * 15, 255 - (x + y) * 7)
            planes[0].append(0.299 * red + 0.587 * green + 0.114 * blue - 128.0)
            planes[1].append(-0.168736 * red - 0.331264 * green + 0.5 * blue)
            planes[2].append(0.5 * red - 0.418688 * green - 0.081312 * blue)
    return jpeg(width, height, planes)


def make_grey_photo() -> bytes:
    return jpeg(8, 8, [[float((x * 32 + y * 8) % 256) - 128.0 for y in range(8) for x in range(8)]])


# ---------------------------------------------------------------------------------------------------------------------
# BMP and TGA
# ---------------------------------------------------------------------------------------------------------------------

def make_bmp() -> bytes:
    width, height = 5, 3
    stride = (width * 3 + 3) // 4 * 4
    pixels = bytearray()
    for y in range(height - 1, -1, -1):  # bottom-up
        row = bytearray()
        for x in range(width):
            red, green, blue = x * 50, y * 120, 200 - x * 40
            row += bytes([blue, green, red])
        pixels += row + bytes(stride - len(row))
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    header = struct.pack("<2sIHHI", b"BM", 14 + len(info) + len(pixels), 0, 0, 14 + len(info))
    return header + info + bytes(pixels)


def tga_header(image_type: int, width: int, height: int, depth: int, descriptor: int) -> bytes:
    return struct.pack("<BBBHHBHHHHBB", 0, 0, image_type, 0, 0, 0, 0, 0, width, height, depth, descriptor)


def make_alpha_tga() -> bytes:
    width, height = 3, 2
    pixels = bytearray()
    for y in range(height - 1, -1, -1):  # bottom-left origin: the bottom row first
        for x in range(width):
            pixels += bytes([x * 100, y * 200, 255 - x * 80, 64 + x * 64])  # B, G, R, A
    return tga_header(2, width, height, 32, 8) + bytes(pixels)


def make_rle_tga() -> bytes:
    """Row 0: a run of three equal pixels and one raw pixel; row 1: one raw packet of four pixels."""
    body = bytearray()
    body += bytes([0x80 | 2]) + bytes([10, 20, 30])
    body += bytes([0]) + bytes([40, 50, 60])
    body += bytes([3]) + bytes([70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180])
    return tga_header(10, 4, 2, 24, 0x20) + bytes(body)


# ---------------------------------------------------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------------------------------------------------

FIXTURES: dict[str, Callable[[], bytes]] = {
    "Rgba.png": make_rgba,
    "Rgb.png": make_rgb,
    "Grey.png": make_grey,
    "GreyAlpha.png": make_grey_alpha,
    "Palette.png": make_palette,
    "Deep.png": make_deep,
    "Normal.png": make_normal,
    "Photo.jpg": make_photo,
    "GreyPhoto.jpg": make_grey_photo,
    "Solid.bmp": make_bmp,
    "Alpha.tga": make_alpha_tga,
    "Rle.tga": make_rle_tga,
    "Truncated.png": make_truncated,
}


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the texture importer fixtures in Tests/Data/Assets/Textures.",
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
    print(f"{'FAILED' if failed else 'passed'}: {len(FIXTURES)} texture fixtures, {len(differences)} {verb}", file=out)
    if options.json:
        document = {"status": "failed" if failed else "passed", "check": options.check, "fixtures": sorted(FIXTURES),
                    "differences": differences, "errors": errors}
        print(json.dumps(document, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
