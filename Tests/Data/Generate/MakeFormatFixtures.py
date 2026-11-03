#!/usr/bin/env python3
"""Generate the first-party format fixtures that no other generator writes: a Radiance HDR environment, its OpenEXR
copies and a TrueType font in Tests/Data/Assets (Roadmap M14, Docs/Decisions/0022-m14-contract.md decision 16; the
OpenEXR environments M23, Docs/Decisions/0034-m23-contract.md decision 14).

Every file is built from its format's specification with the standard library only. Every value is a function of its
position, computed with exact integer arithmetic or IEEE double operations in a fixed order (no transcendental
function), so the bytes are identical on every platform and Python 3.10+. Nothing is downloaded and nothing is random;
the ZIP data is deflated by this script's own encoder (one fixed-Huffman block, matches found by a deterministic
search), never by the zlib Python links, whose output can differ between builds.

The fixtures (paths relative to Tests/Data/Assets):
  Environments/Gradient.hdr  32 x 16 equirectangular sky gradient (2:1, as the environment importer requires) with one
                             bright sun texel, in flat (not run-length encoded) RGBE scanlines
  Environments/Gradient.exr  the same texels as Gradient.hdr decodes them (each RGBE value is m * 2^(e - 136), exact
                             in a binary16), as a single-part OpenEXR image of half-float B, G and R scanlines,
                             uncompressed
  Environments/GradientZip.exr
                             the same image with ZIP compression (one block of 16 scanlines: OpenEXR's byte
                             interleave and delta predictor, then a zlib stream)
  Environments/GradientBlocks<Compression>.exr
                             Gradient.hdr's texels each repeated over 4 x 4 texels (128 x 64), so the image holds
                             several blocks of every compression the environment importer reads: None, Rle and Zips
                             (64 blocks of one scanline), Zip and Pxr24 (four of 16) and Piz (two of 32); RLE is the
                             interleave and predictor with OpenEXR's run-length code, PXR24 each scanline's channels
                             as delta-coded byte planes in a zlib stream, and PIZ the bitmap and lookup table of the
                             values used, the 14-bit Haar wavelet and OpenEXR's 16-bit Huffman code with its packed
                             table (a canonical code from code lengths this script's own Huffman construction finds)
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
import heapq
import json
import math
import struct
import sys
import zlib
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
# OpenEXR (the OpenEXR file layout: magic number, version, header attributes, offset table, chunks)
# ---------------------------------------------------------------------------------------------------------------------

EXR_COMPRESSION_NONE = 0
EXR_COMPRESSION_RLE = 1
EXR_COMPRESSION_ZIPS = 2
EXR_COMPRESSION_ZIP = 3
EXR_COMPRESSION_PIZ = 4
EXR_COMPRESSION_PXR24 = 5
EXR_PIXEL_HALF = 1
EXR_CHANNELS = ("B", "G", "R")  # sorted by name, as OpenEXR stores them
# The scanlines of one chunk, by compression (OpenEXR's numbers).
EXR_LINES_PER_BLOCK = {EXR_COMPRESSION_NONE: 1, EXR_COMPRESSION_RLE: 1, EXR_COMPRESSION_ZIPS: 1,
                       EXR_COMPRESSION_ZIP: 16, EXR_COMPRESSION_PIZ: 32, EXR_COMPRESSION_PXR24: 16}
# The block fixtures repeat each of Gradient.hdr's texels over BLOCK_SCALE x BLOCK_SCALE texels.
BLOCK_SCALE = 4


def decoded_hdr_texel(column: int, row: int) -> tuple[float, float, float]:
    """The texel Gradient.hdr holds as stb_image decodes it: each mantissa byte times 2^(exponent - 136) (zero for a
    zero exponent byte), exact in a binary16 for every texel of the image."""
    encoded = rgbe(*sky_radiance(column, row))
    if encoded[3] == 0:
        return (0.0, 0.0, 0.0)
    scale = 2.0 ** (encoded[3] - 136)
    return (encoded[0] * scale, encoded[1] * scale, encoded[2] * scale)


def exr_attribute(name: str, kind: str, value: bytes) -> bytes:
    return name.encode("ascii") + b"\0" + kind.encode("ascii") + b"\0" + struct.pack("<i", len(value)) + value


def exr_scanline(row: int, scale: int = 1) -> bytes:
    """One scanline's pixel data: each channel's HDR_WIDTH * scale half floats in turn, in the header's channel order;
    texel (column, row) is Gradient.hdr's (column // scale, row // scale)."""
    texels = [decoded_hdr_texel(column // scale, row // scale) for column in range(HDR_WIDTH * scale)]
    data = bytearray()
    for channel in EXR_CHANNELS:
        component = "RGB".index(channel)
        for texel in texels:
            half = struct.pack("<e", texel[component])
            if struct.unpack("<e", half)[0] != texel[component]:
                raise ValueError(f"texel value {texel[component]} of row {row} is not exact in a binary16")
            data += half
    return bytes(data)


class BitWriter:
    """Deflate's bit order (RFC 1951 section 3.1.1): values least significant bit first, Huffman codes most significant
    bit first."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.value = 0
        self.count = 0

    def bits(self, value: int, count: int) -> None:
        self.value |= value << self.count
        self.count += count
        while self.count >= 8:
            self.data.append(self.value & 0xFF)
            self.value >>= 8
            self.count -= 8

    def code(self, code: int, length: int) -> None:
        reversed_code = 0
        for _ in range(length):
            reversed_code = (reversed_code << 1) | (code & 1)
            code >>= 1
        self.bits(reversed_code, length)

    def finish(self) -> bytes:
        if self.count > 0:
            self.data.append(self.value & 0xFF)
        return bytes(self.data)


# Deflate's length and distance codes (RFC 1951 section 3.2.5): (first value, extra bits) of each code.
DEFLATE_LENGTHS = [(3, 0), (4, 0), (5, 0), (6, 0), (7, 0), (8, 0), (9, 0), (10, 0), (11, 1), (13, 1), (15, 1), (17, 1),
                   (19, 2), (23, 2), (27, 2), (31, 2), (35, 3), (43, 3), (51, 3), (59, 3), (67, 4), (83, 4), (99, 4),
                   (115, 4), (131, 5), (163, 5), (195, 5), (227, 5), (258, 0)]
DEFLATE_DISTANCES = [(1, 0), (2, 0), (3, 0), (4, 0), (5, 1), (7, 1), (9, 2), (13, 2), (17, 3), (25, 3), (33, 4),
                     (49, 4), (65, 5), (97, 5), (129, 6), (193, 6), (257, 7), (385, 7), (513, 8), (769, 8), (1025, 9),
                     (1537, 9), (2049, 10), (3073, 10), (4097, 11), (6145, 11), (8193, 12), (12289, 12), (16385, 13),
                     (24577, 13)]
DEFLATE_WINDOW = 32768
DEFLATE_MAX_MATCH = 258


def fixed_literal(writer: BitWriter, symbol: int) -> None:
    """A literal or length symbol in the fixed Huffman code (RFC 1951 section 3.2.6)."""
    if symbol < 144:
        writer.code(0x30 + symbol, 8)
    elif symbol < 256:
        writer.code(0x190 + symbol - 144, 9)
    elif symbol < 280:
        writer.code(symbol - 256, 7)
    else:
        writer.code(0xC0 + symbol - 280, 8)


def zlib_fixed(data: bytes) -> bytes:
    """A zlib stream (RFC 1950) of one fixed-Huffman deflate block: at each position the longest earlier match of at
    least 3 bytes (the nearest of equal length) within the window, else a literal. Identical on every Python build."""
    writer = BitWriter()
    writer.bits(1, 1)  # BFINAL
    writer.bits(1, 2)  # BTYPE 01: fixed Huffman codes
    candidates: dict[bytes, list[int]] = {}
    position = 0
    while position < len(data):
        best_length = 0
        best_distance = 0
        key = data[position:position + 3]
        if len(key) == 3:
            for start in reversed(candidates.get(key, [])):
                if position - start > DEFLATE_WINDOW:
                    break
                length = 0
                while (length < DEFLATE_MAX_MATCH and position + length < len(data)
                       and data[start + length] == data[position + length]):
                    length += 1
                if length > best_length:
                    best_length = length
                    best_distance = position - start
        step = best_length if best_length >= 3 else 1
        if step >= 3:
            code = max(index for index, (base, _) in enumerate(DEFLATE_LENGTHS) if base <= best_length)
            base, extra = DEFLATE_LENGTHS[code]
            fixed_literal(writer, 257 + code)
            writer.bits(best_length - base, extra)
            code = max(index for index, (base, _) in enumerate(DEFLATE_DISTANCES) if base <= best_distance)
            base, extra = DEFLATE_DISTANCES[code]
            writer.code(code, 5)
            writer.bits(best_distance - base, extra)
        else:
            fixed_literal(writer, data[position])
        for index in range(position, position + step):
            if index + 3 <= len(data):
                candidates.setdefault(data[index:index + 3], []).append(index)
        position += step
    fixed_literal(writer, 256)  # end of block
    return b"\x78\x01" + writer.finish() + struct.pack(">I", zlib.adler32(data) & 0xFFFFFFFF)


def exr_predicted(raw: bytes) -> bytes:
    """OpenEXR's ZIP and RLE preprocessing: the bytes interleaved (even offsets, then odd), delta-coded from the second
    byte on ((byte - previous + 128) mod 256)."""
    interleaved = bytearray(raw[0::2] + raw[1::2])
    predicted = bytearray(interleaved)
    for index in range(1, len(interleaved)):
        predicted[index] = (interleaved[index] - interleaved[index - 1] + 128) & 0xFF
    return bytes(predicted)


def exr_zip(raw: bytes) -> bytes:
    """OpenEXR's ZIP and ZIPS chunk data: the predicted bytes (exr_predicted), deflated."""
    return zlib_fixed(exr_predicted(raw))


def exr_rle(raw: bytes) -> bytes:
    """OpenEXR's RLE chunk data: the predicted bytes as runs of 3 to 128 equal bytes (a count byte n - 1 and the byte)
    and literal stretches of up to 127 bytes (a count byte -n and the bytes)."""
    predicted = exr_predicted(raw)
    out = bytearray()
    position = 0
    while position < len(predicted):
        run = 1
        while position + run < len(predicted) and run < 128 and predicted[position + run] == predicted[position]:
            run += 1
        if run >= 3:
            out += bytes([run - 1, predicted[position]])
            position += run
            continue
        literal = 0
        while position + literal < len(predicted) and literal < 127:
            at = position + literal
            if at + 2 < len(predicted) and predicted[at] == predicted[at + 1] == predicted[at + 2]:
                break
            literal += 1
        out += bytes([-literal & 0xFF]) + predicted[position:position + literal]
        position += literal
    return bytes(out)


def exr_pxr24(raw: bytes, width: int) -> bytes:
    """OpenEXR's PXR24 chunk data for half channels: each scanline's channels in turn, each channel's values delta-coded
    from 0 (mod 2^16) and split into a plane of their high bytes and a plane of their low bytes, then deflated."""
    planes = bytearray()
    for offset in range(0, len(raw), 2 * width):
        values = struct.unpack(f"<{width}H", raw[offset:offset + 2 * width])
        previous = 0
        differences = []
        for value in values:
            differences.append((value - previous) & 0xFFFF)
            previous = value
        planes += bytes(difference >> 8 for difference in differences)
        planes += bytes(difference & 0xFF for difference in differences)
    return zlib_fixed(bytes(planes))


# OpenEXR's 16-bit Huffman code (PIZ): code lengths of at most 58 bits, packed as 6-bit lengths with runs of zero
# lengths.
HUF_SHORT_ZEROCODE_RUN = 59
HUF_LONG_ZEROCODE_RUN = 63
HUF_SHORTEST_LONG_RUN = 2 + HUF_LONG_ZEROCODE_RUN - HUF_SHORT_ZEROCODE_RUN
HUF_LONGEST_LONG_RUN = 255 + HUF_SHORTEST_LONG_RUN
HUF_MAX_CODE_LENGTH = 58


class MsbBitWriter:
    """OpenEXR's Huffman bit order: values most significant bit first, the last byte padded with zero bits."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.value = 0
        self.count = 0

    def bits(self, count: int, value: int) -> None:
        self.value = (self.value << count) | value
        self.count += count
        while self.count >= 8:
            self.count -= 8
            self.data.append((self.value >> self.count) & 0xFF)
        self.value &= (1 << self.count) - 1

    def finish(self) -> tuple[bytes, int]:
        """The bytes and the number of bits written."""
        bit_count = len(self.data) * 8 + self.count
        tail = bytes([(self.value << (8 - self.count)) & 0xFF]) if self.count > 0 else b""
        return bytes(self.data) + tail, bit_count


def huffman_lengths(frequencies: dict[int, int]) -> dict[int, int]:
    """The code length of every symbol of a Huffman code for `frequencies`: the two least frequent nodes merged first,
    ties broken by creation order (the symbols in increasing order, then the merged nodes as they are made)."""
    heap: list[tuple[int, int, list[int]]] = []
    for order, symbol in enumerate(sorted(frequencies)):
        heapq.heappush(heap, (frequencies[symbol], order, [symbol]))
    lengths = dict.fromkeys(frequencies, 0)
    order = len(heap)
    while len(heap) > 1:
        first_frequency, _, first = heapq.heappop(heap)
        second_frequency, _, second = heapq.heappop(heap)
        for symbol in first + second:
            lengths[symbol] += 1
        heapq.heappush(heap, (first_frequency + second_frequency, order, first + second))
        order += 1
    if max(lengths.values()) > HUF_MAX_CODE_LENGTH:
        raise ValueError("a Huffman code is longer than OpenEXR's 58 bits")
    return lengths


def huffman_codes(lengths: dict[int, int]) -> dict[int, int]:
    """OpenEXR's canonical code for `lengths` (the decoder builds the same from the lengths alone): the longest codes
    are numerically lowest, and codes of one length increase with the symbol."""
    counts = [0] * (HUF_MAX_CODE_LENGTH + 1)
    for length in lengths.values():
        counts[length] += 1
    first = [0] * (HUF_MAX_CODE_LENGTH + 1)
    code = 0
    for length in range(HUF_MAX_CODE_LENGTH, 0, -1):
        first[length] = code
        code = (code + counts[length]) >> 1
    codes = {}
    for symbol in sorted(lengths):
        codes[symbol] = first[lengths[symbol]]
        first[lengths[symbol]] += 1
    return codes


def huffman_compress(values: list[int]) -> bytes:
    """OpenEXR's 16-bit Huffman stream: its header (the smallest symbol; the largest, the run-length symbol one past the
    largest value; the packed table's length; the bit count; a zero word), the table and the codes. A run of a value is
    sent as the value's code, the run-length symbol's code and an 8-bit repeat count when that is shorter."""
    frequencies: dict[int, int] = {}
    for value in values:
        frequencies[value] = frequencies.get(value, 0) + 1
    smallest = min(frequencies)
    run_symbol = max(frequencies) + 1
    frequencies[run_symbol] = 1
    lengths = huffman_lengths(frequencies)
    codes = huffman_codes(lengths)

    table = MsbBitWriter()
    symbol = smallest
    while symbol <= run_symbol:
        if lengths.get(symbol, 0) == 0:
            zeros = 1
            while symbol < run_symbol and zeros < HUF_LONGEST_LONG_RUN and lengths.get(symbol + 1, 0) == 0:
                symbol += 1
                zeros += 1
            if zeros >= HUF_SHORTEST_LONG_RUN:
                table.bits(6, HUF_LONG_ZEROCODE_RUN)
                table.bits(8, zeros - HUF_SHORTEST_LONG_RUN)
            elif zeros >= 2:
                table.bits(6, HUF_SHORT_ZEROCODE_RUN + zeros - 2)
            else:
                table.bits(6, 0)
        else:
            table.bits(6, lengths[symbol])
        symbol += 1
    table_bytes, _ = table.finish()

    stream = MsbBitWriter()

    def send(value: int, repeats: int) -> None:
        if lengths[value] + lengths[run_symbol] + 8 < lengths[value] * repeats:
            stream.bits(lengths[value], codes[value])
            stream.bits(lengths[run_symbol], codes[run_symbol])
            stream.bits(8, repeats)
        else:
            for _ in range(repeats + 1):
                stream.bits(lengths[value], codes[value])

    current = values[0]
    repeats = 0
    for value in values[1:]:
        if value == current and repeats < 255:
            repeats += 1
        else:
            send(current, repeats)
            repeats = 0
        current = value
    send(current, repeats)
    stream_bytes, bit_count = stream.finish()
    return struct.pack("<IIIII", smallest, run_symbol, len(table_bytes), bit_count, 0) + table_bytes + stream_bytes


def haar14(a: int, b: int) -> tuple[int, int]:
    """OpenEXR's 14-bit Haar step: the mean and the difference of two values, as 16-bit two's complement."""
    signed_a = a - 0x10000 if a >= 0x8000 else a
    signed_b = b - 0x10000 if b >= 0x8000 else b
    return ((signed_a + signed_b) >> 1) & 0xFFFF, (signed_a - signed_b) & 0xFFFF


def wavelet_encode(values: list[int], start: int, width: int, height: int) -> None:
    """OpenEXR's 2D wavelet encoding of the `width` x `height` values at `start` (rows of `width`), in place, for values
    below 2^14: each level pairs columns and rows `level` apart, then the odd column and the odd row left over."""
    level = 1
    span = 2
    shortest = min(width, height)
    while span <= shortest:
        row = start
        last_row = start + width * (height - span)
        while row <= last_row:
            column = row
            last_column = row + (width - span)
            while column <= last_column:
                right = column + level
                below = column + width * level
                diagonal = below + level
                mean_top, difference_top = haar14(values[column], values[right])
                mean_bottom, difference_bottom = haar14(values[below], values[diagonal])
                values[column], values[below] = haar14(mean_top, mean_bottom)
                values[right], values[diagonal] = haar14(difference_top, difference_bottom)
                column += span
            if width & level:
                below = column + width * level
                values[column], values[below] = haar14(values[column], values[below])
            row += width * span
        if height & level:
            column = row
            last_column = row + (width - span)
            while column <= last_column:
                right = column + level
                values[column], values[right] = haar14(values[column], values[right])
                column += span
        level = span
        span <<= 1


def exr_piz(raw: bytes, width: int, lines: int) -> bytes:
    """OpenEXR's PIZ chunk data for half channels: the values regrouped by channel, the range of the bitmap of the
    values used (zero is implied), mapped through the lookup table of that bitmap, wavelet-encoded per channel and
    Huffman-compressed after a length word."""
    channel_count = len(raw) // (2 * width * lines)
    line_values = struct.unpack(f"<{len(raw) // 2}H", raw)
    values: list[int] = []
    for channel in range(channel_count):
        for line in range(lines):
            first = (line * channel_count + channel) * width
            values += line_values[first:first + width]
    bitmap = bytearray(8192)
    for value in values:
        bitmap[value >> 3] |= 1 << (value & 7)
    bitmap[0] &= 0xFE
    used = [index for index, byte in enumerate(bitmap) if byte != 0]
    minimum, maximum = (used[0], used[-1]) if used else (8191, 0)
    lookup: dict[int, int] = {0: 0}
    for value in range(1, 1 << 16):
        if bitmap[value >> 3] & (1 << (value & 7)):
            lookup[value] = len(lookup)
    if len(lookup) - 1 >= 1 << 14:
        raise ValueError("the PIZ fixture needs the 14-bit wavelet: fewer than 2^14 distinct values")
    values = [lookup[value] for value in values]
    for channel in range(channel_count):
        wavelet_encode(values, channel * width * lines, width, lines)
    compressed = huffman_compress(values)
    header = struct.pack("<HH", minimum, maximum) + (bytes(bitmap[minimum:maximum + 1]) if used else b"")
    return header + struct.pack("<i", len(compressed)) + compressed


def exr_compress(compression: int, raw: bytes, width: int, lines: int) -> bytes:
    """A chunk's pixel data under `compression` (NONE: `raw` itself)."""
    if compression == EXR_COMPRESSION_NONE:
        return raw
    if compression == EXR_COMPRESSION_RLE:
        return exr_rle(raw)
    if compression in (EXR_COMPRESSION_ZIPS, EXR_COMPRESSION_ZIP):
        return exr_zip(raw)
    if compression == EXR_COMPRESSION_PIZ:
        return exr_piz(raw, width, lines)
    if compression == EXR_COMPRESSION_PXR24:
        return exr_pxr24(raw, width)
    raise ValueError(f"compression {compression} is not written")


def make_gradient_exr(compression: int, scale: int = 1) -> bytes:
    """Gradient.hdr's texels, each repeated over `scale` x `scale` texels, as a single-part scanline OpenEXR image of
    half-float B, G and R channels."""
    width = HDR_WIDTH * scale
    height = HDR_HEIGHT * scale
    channels = b"".join(name.encode("ascii") + b"\0" + struct.pack("<iB3xii", EXR_PIXEL_HALF, 0, 1, 1)
                        for name in EXR_CHANNELS) + b"\0"
    window = struct.pack("<iiii", 0, 0, width - 1, height - 1)
    header = (exr_attribute("channels", "chlist", channels)
              + exr_attribute("compression", "compression", bytes([compression]))
              + exr_attribute("dataWindow", "box2i", window)
              + exr_attribute("displayWindow", "box2i", window)
              + exr_attribute("lineOrder", "lineOrder", b"\0")
              + exr_attribute("pixelAspectRatio", "float", struct.pack("<f", 1.0))
              + exr_attribute("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0))
              + exr_attribute("screenWindowWidth", "float", struct.pack("<f", 1.0))
              + b"\0")
    preamble = b"\x76\x2f\x31\x01" + struct.pack("<I", 2) + header  # magic number; version 2, single-part scanlines
    lines = EXR_LINES_PER_BLOCK[compression]
    chunks: list[bytes] = []
    for first in range(0, height, lines):
        block_lines = min(lines, height - first)
        raw = b"".join(exr_scanline(row, scale) for row in range(first, first + block_lines))
        data = exr_compress(compression, raw, width, block_lines)
        # A chunk no smaller than its pixels would read as stored uncompressed.
        if compression != EXR_COMPRESSION_NONE and len(data) >= len(raw):
            raise ValueError(f"the block of line {first} does not compress ({len(data)} of {len(raw)} bytes)")
        chunks.append(struct.pack("<ii", first, len(data)) + data)
    offset = len(preamble) + 8 * len(chunks)
    table = bytearray()
    for chunk in chunks:
        table += struct.pack("<Q", offset)
        offset += len(chunk)
    return preamble + bytes(table) + b"".join(chunks)


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
    "Environments/Gradient.exr": lambda: make_gradient_exr(EXR_COMPRESSION_NONE),
    "Environments/GradientZip.exr": lambda: make_gradient_exr(EXR_COMPRESSION_ZIP),
    "Environments/GradientBlocksNone.exr": lambda: make_gradient_exr(EXR_COMPRESSION_NONE, BLOCK_SCALE),
    "Environments/GradientBlocksRle.exr": lambda: make_gradient_exr(EXR_COMPRESSION_RLE, BLOCK_SCALE),
    "Environments/GradientBlocksZips.exr": lambda: make_gradient_exr(EXR_COMPRESSION_ZIPS, BLOCK_SCALE),
    "Environments/GradientBlocksZip.exr": lambda: make_gradient_exr(EXR_COMPRESSION_ZIP, BLOCK_SCALE),
    "Environments/GradientBlocksPiz.exr": lambda: make_gradient_exr(EXR_COMPRESSION_PIZ, BLOCK_SCALE),
    "Environments/GradientBlocksPxr24.exr": lambda: make_gradient_exr(EXR_COMPRESSION_PXR24, BLOCK_SCALE),
    "Fonts/Glyphs.ttf": make_glyphs_ttf,
}


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the Radiance HDR, OpenEXR and TrueType format fixtures in Tests/Data/Assets.",
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
