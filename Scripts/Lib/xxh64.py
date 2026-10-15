"""Seed-zero XXH64, the hash of every "XXH64" member of a project's Automation/Provenance.json (Docs/Architecture.md
§13.4, §13.12; Docs/Decisions/0026-m16-contract.md decision 5, stream A).

A byte-identical port of Tools/MCP/engine_mcp/autosave.py's xxh64, which matches Engine/Core/Hash.cpp: Scripts/ cannot
import the MCP package, which lives on Tools/MCP/.venv's import path only. Both copies run the reference vectors of
Tests/Source/Engine/Core/HashTests.cpp (Tests/Automation/test_audit_provenance.py and
Tools/MCP/tests/test_autosave_probe.py), so they cannot drift apart unnoticed. Standard library only.
"""

from __future__ import annotations

import struct

_MASK = (1 << 64) - 1
_PRIME1 = 0x9E3779B185EBCA87
_PRIME2 = 0xC2B2AE3D27D4EB4F
_PRIME3 = 0x165667B19E3779F9
_PRIME4 = 0x85EBCA77C2B2AE63
_PRIME5 = 0x27D4EB2F165667C5


def _rotate(value: int, count: int) -> int:
    value &= _MASK
    return ((value << count) | (value >> (64 - count))) & _MASK


def _round(accumulator: int, lane: int) -> int:
    return (_rotate(accumulator + lane * _PRIME2, 31) * _PRIME1) & _MASK


def xxh64(data: bytes) -> int:
    """The seed-zero XXH64 of `data`, as Engine/Core/Hash.cpp's XXH64 computes it (all of XXH64's tail paths: 32-byte
    stripes, 8-byte, 4-byte and single-byte tails)."""
    offset = 0
    if len(data) >= 32:
        lanes = [(_PRIME1 + _PRIME2) & _MASK, _PRIME2, 0, (-_PRIME1) & _MASK]
        while offset + 32 <= len(data):
            for index, word in enumerate(struct.unpack_from("<4Q", data, offset)):
                lanes[index] = _round(lanes[index], word)
            offset += 32
        result = sum(_rotate(value, shift) for value, shift in zip(lanes, (1, 7, 12, 18))) & _MASK
        for value in lanes:
            result = ((result ^ _round(0, value)) * _PRIME1 + _PRIME4) & _MASK
    else:
        result = _PRIME5
    result = (result + len(data)) & _MASK
    while offset + 8 <= len(data):
        result ^= _round(0, struct.unpack_from("<Q", data, offset)[0])
        result = (_rotate(result, 27) * _PRIME1 + _PRIME4) & _MASK
        offset += 8
    if offset + 4 <= len(data):
        result ^= struct.unpack_from("<I", data, offset)[0] * _PRIME1
        result = (_rotate(result, 23) * _PRIME2 + _PRIME3) & _MASK
        offset += 4
    for byte in data[offset:]:
        result ^= byte * _PRIME5
        result = (_rotate(result, 11) * _PRIME1) & _MASK
    result = ((result ^ (result >> 33)) * _PRIME2) & _MASK
    result = ((result ^ (result >> 29)) * _PRIME3) & _MASK
    return result ^ (result >> 32)


def xxh64_hex(data: bytes) -> str:
    """xxh64(data) as Provenance.json writes it (ProvenanceRecorder.h): 16 lowercase hexadecimal digits, zero-padded."""
    return f"{xxh64(data):016x}"
