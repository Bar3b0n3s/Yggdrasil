"""Seed-zero XXH64, the hash of every "XXH64" member of a project's Automation/Provenance.json (Docs/Architecture.md
§13.4, §13.12; Docs/Decisions/0026-m16-contract.md decision 5, stream A).

A byte-identical port of Tools/MCP/engine_mcp/autosave.py's xxh64, which matches Engine/Core/Hash.cpp: Scripts/ cannot
import the MCP package, which lives on Tools/MCP/.venv's import path only. Both copies run the reference vectors of
Tests/Source/Engine/Core/HashTests.cpp (Tests/Automation/test_audit_provenance.py and
Tools/MCP/tests/test_autosave_probe.py), so they cannot drift apart unnoticed. Standard library only.
"""

from __future__ import annotations


def xxh64(data: bytes) -> int:
    """The seed-zero XXH64 of `data`, as Engine/Core/Hash.cpp's XXH64 computes it (all of XXH64's tail paths: 32-byte
    stripes, 8-byte, 4-byte and single-byte tails)."""
    raise NotImplementedError("contract stub: xxh64 (ADR 0026 decision 5, stream A)")


def xxh64_hex(data: bytes) -> str:
    """xxh64(data) as Provenance.json writes it (ProvenanceRecorder.h): 16 lowercase hexadecimal digits, zero-padded."""
    raise NotImplementedError("contract stub: xxh64 (ADR 0026 decision 5, stream A)")
