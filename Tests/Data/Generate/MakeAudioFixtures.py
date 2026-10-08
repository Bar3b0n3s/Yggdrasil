#!/usr/bin/env python3
"""Generate the audio decoder and importer fixtures in Tests/Data/Assets/Audio (Roadmap M12,
Docs/Decisions/0015-m12-decisions.md decision 18).

Both files are built from their format's specification with the standard library only. The tones are computed without
the C library's sine (which differs between platforms): each sample's phase is reduced exactly in integers, and the sine
of the reduced angle is a fixed Taylor polynomial evaluated with IEEE double operations in a fixed order, so the bytes
are identical on every platform and Python 3.10+. Nothing is downloaded and nothing is random.

The fixtures (paths relative to Tests/Data/Assets/Audio):
  Tone.wav   0.1 s at 44.1 kHz, stereo, 16-bit PCM (RIFF/WAVE, canonical 44-byte header): 440 Hz left, 660 Hz right
  Tone.flac  0.1 s at 48 kHz, mono, 16-bit FLAC with verbatim subframes (a STREAMINFO block with the MD5 of the
             samples, frames of 4096 samples with their CRC-8 and CRC-16): 440 Hz

Tone.mp3 and Tone.ogg in the same directory are third-party files pinned in Tests/Data/LICENSES.md; this script neither
writes nor checks them.

Run from the repository root: `python Tests/Data/Generate/MakeAudioFixtures.py` writes the files; `--check` compares a
fresh generation with the committed files and writes nothing; `--json` prints a machine-readable report on stdout.

Exit codes: 0 success, 1 --check found a difference or a file could not be written, 2 usage error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from collections.abc import Callable
from pathlib import Path

OUTPUT_DIRECTORY = Path(__file__).resolve().parents[1] / "Assets" / "Audio"

AMPLITUDE = 0.5
FULL_SCALE = 32767.0

# ---------------------------------------------------------------------------------------------------------------------
# Tones
# ---------------------------------------------------------------------------------------------------------------------


def taylor_sine(angle: float) -> float:
    """sin(angle) for |angle| <= pi / 2: the Taylor series through the 13th power in nested form (error below 1e-9)."""
    square = angle * angle
    value = 1.0 - square / 156.0
    value = 1.0 - square / 110.0 * value
    value = 1.0 - square / 72.0 * value
    value = 1.0 - square / 42.0 * value
    value = 1.0 - square / 20.0 * value
    value = 1.0 - square / 6.0 * value
    return angle * value


def tone_sample(frame: int, frequency: int, sample_rate: int) -> float:
    """sin(2 pi frequency frame / sample_rate), with the phase reduced exactly to [-pi / 2, pi / 2]."""
    phase = (frame * frequency) % sample_rate  # in units of 1 / sample_rate turns, [0, sample_rate)
    quarter = sample_rate / 4
    if phase <= quarter:
        reduced = phase
    elif phase <= 3 * quarter:
        reduced = sample_rate / 2 - phase  # sin(pi - x) = sin(x)
    else:
        reduced = phase - sample_rate
    return taylor_sine(reduced * (2.0 * math.pi) / sample_rate)


def quantize(value: float) -> int:
    """A sample of full scale 32767, rounded half away from zero."""
    scaled = value * FULL_SCALE
    return math.floor(scaled + 0.5) if scaled >= 0.0 else -math.floor(-scaled + 0.5)


def tone_frames(sample_rate: int, frame_count: int, frequencies: list[int]) -> list[tuple[int, ...]]:
    """`frame_count` frames with one channel per frequency, each a tone of AMPLITUDE."""
    return [tuple(quantize(AMPLITUDE * tone_sample(frame, frequency, sample_rate)) for frequency in frequencies)
            for frame in range(frame_count)]


def pcm16_little_endian(frames: list[tuple[int, ...]]) -> bytes:
    return b"".join(struct.pack("<" + "h" * len(frame), *frame) for frame in frames)


# ---------------------------------------------------------------------------------------------------------------------
# WAV
# ---------------------------------------------------------------------------------------------------------------------


def wav_file(sample_rate: int, frames: list[tuple[int, ...]]) -> bytes:
    """A canonical RIFF/WAVE file: a 16-byte PCM fmt chunk and the data chunk."""
    channels = len(frames[0])
    data = pcm16_little_endian(frames)
    block_align = channels * 2
    fmt = struct.pack("<HHIIHH", 1, channels, sample_rate, sample_rate * block_align, block_align, 16)
    return (b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE"
            + b"fmt " + struct.pack("<I", len(fmt)) + fmt
            + b"data" + struct.pack("<I", len(data)) + data)


def make_wav() -> bytes:
    return wav_file(44100, tone_frames(44100, 4410, [440, 660]))


# ---------------------------------------------------------------------------------------------------------------------
# FLAC
# ---------------------------------------------------------------------------------------------------------------------

FLAC_BLOCK_SIZE = 4096
FLAC_SAMPLE_RATE_CODES = {44100: 0b1001, 48000: 0b1010}
FLAC_SAMPLE_SIZE_16 = 0b100
FLAC_BLOCK_SIZE_16_BIT = 0b0111  # the block size minus one follows the header as 16 bits
FLAC_SUBFRAME_VERBATIM = 0b000001


def crc8(data: bytes) -> int:
    """FLAC's frame header CRC: polynomial x^8 + x^2 + x + 1, initial value 0."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crc16(data: bytes) -> int:
    """FLAC's frame CRC: polynomial x^16 + x^15 + x^2 + 1, initial value 0."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def flac_frame_number(number: int) -> bytes:
    """The frame number in FLAC's UTF-8-like coding (the frames here number fewer than 128)."""
    if number >= 0x80:
        raise ValueError("frame numbers of 128 and above are not needed by these fixtures")
    return bytes([number])


def flac_frame(number: int, sample_rate: int, frames: list[tuple[int, ...]]) -> bytes:
    """One fixed-blocksize frame with a verbatim subframe per channel."""
    channels = len(frames[0])
    header = bytearray(b"\xFF\xF8")  # sync code, reserved bit, fixed blocking strategy
    header.append((FLAC_BLOCK_SIZE_16_BIT << 4) | FLAC_SAMPLE_RATE_CODES[sample_rate])
    header.append(((channels - 1) << 4) | (FLAC_SAMPLE_SIZE_16 << 1))  # independent channels, 16 bits, reserved bit
    header += flac_frame_number(number)
    header += struct.pack(">H", len(frames) - 1)
    header.append(crc8(bytes(header)))

    body = bytearray(header)
    for channel in range(channels):
        body.append(FLAC_SUBFRAME_VERBATIM << 1)  # zero padding bit, subframe type, no wasted bits
        body += b"".join(struct.pack(">h", frame[channel]) for frame in frames)
    body += struct.pack(">H", crc16(bytes(body)))
    return bytes(body)


def flac_file(sample_rate: int, frames: list[tuple[int, ...]]) -> bytes:
    """The "fLaC" marker, the STREAMINFO metadata block (the last one) and the frames."""
    channels = len(frames[0])
    blocks = [frames[start:start + FLAC_BLOCK_SIZE] for start in range(0, len(frames), FLAC_BLOCK_SIZE)]
    encoded = [flac_frame(number, sample_rate, block) for number, block in enumerate(blocks)]
    sizes = [len(frame) for frame in encoded]

    stream_info = struct.pack(">HH", FLAC_BLOCK_SIZE, FLAC_BLOCK_SIZE)
    stream_info += min(sizes).to_bytes(3, "big") + max(sizes).to_bytes(3, "big")
    packed = (sample_rate << 44) | ((channels - 1) << 41) | ((16 - 1) << 36) | len(frames)
    stream_info += struct.pack(">Q", packed)
    stream_info += hashlib.md5(pcm16_little_endian(frames)).digest()

    metadata = bytes([0x80 | 0]) + len(stream_info).to_bytes(3, "big") + stream_info  # last block, type STREAMINFO
    return b"fLaC" + metadata + b"".join(encoded)


def make_flac() -> bytes:
    return flac_file(48000, tone_frames(48000, 4800, [440]))


# ---------------------------------------------------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------------------------------------------------

FIXTURES: dict[str, Callable[[], bytes]] = {
    "Tone.wav": make_wav,
    "Tone.flac": make_flac,
}


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the audio decoder and importer fixtures in Tests/Data/Assets/Audio.",
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
    print(f"{'FAILED' if failed else 'passed'}: {len(FIXTURES)} audio fixtures, {len(differences)} {verb}", file=out)
    if options.json:
        document = {"status": "failed" if failed else "passed", "check": options.check, "fixtures": sorted(FIXTURES),
                    "differences": differences, "errors": errors}
        print(json.dumps(document, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
