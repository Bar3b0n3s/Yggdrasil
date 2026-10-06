"""Seeded defects for the contract step's Python stub rule (Scripts/Lint.py, Tests/Data/Lint/Fixtures.json)."""

from __future__ import annotations


def connect() -> None:
    raise NotImplementedError("contract stub: connect")


def abstract() -> None:
    # The allowed control: a NotImplementedError with another message.
    raise NotImplementedError("subclasses implement this")


def mention() -> str:
    # The allowed control: a string that spells the marker.
    return 'raise NotImplementedError("contract stub")'


def formatted(name: str) -> None:
    raise NotImplementedError(f"contract stub: {name}")
