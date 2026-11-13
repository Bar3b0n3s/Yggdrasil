#!/usr/bin/env python3
"""Generate the colour-grading LUT fixtures in Tests/Data/Assets/Luts (Roadmap M26, R-LUT;
Docs/Decisions/0037-m26-contract.md decision 15).

Each file is an Adobe/IRIDAS `.cube` text file (a 3D table, red fastest) written from its specification with the
standard library only. Every value is a rational function of its grid coordinates, computed in exact integer arithmetic
and written with six decimals rounded half up, so the bytes are identical on every platform and Python 3.10+. Nothing is
downloaded and nothing is random.

The fixtures (paths relative to Tests/Data/Assets/Luts):
  Warm.cube  a 9 x 9 x 9 warm grade over the domain [0, 1]: entry (r, g, b) of the grid's coordinates x = r / 8,
             y = g / 8 and z = b / 8 is (0.1 + 0.9 x, y, 0.85 z), with a TITLE line, a comment, DOMAIN_MIN and
             DOMAIN_MAX

Run from the repository root: `python Tests/Data/Generate/MakeLutFixtures.py` writes the files; `--check` compares a
fresh generation with the committed files and writes nothing; `--json` prints a machine-readable report on stdout.

Exit codes: 0 success, 1 --check found a difference or a file could not be written, 2 usage error.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Callable
from fractions import Fraction
from pathlib import Path

OUTPUT_DIRECTORY = Path(__file__).resolve().parents[1] / "Assets" / "Luts"

WARM_SIZE = 9


def format_value(value: Fraction) -> str:
    """`value` (non-negative) with six decimals, rounded half up in exact integer arithmetic."""
    millionths = (value.numerator * 1_000_000 * 2 + value.denominator) // (2 * value.denominator)
    return f"{millionths // 1_000_000}.{millionths % 1_000_000:06d}"


def make_warm_cube() -> bytes:
    """The Warm.cube fixture (see the module docstring)."""
    last = WARM_SIZE - 1
    lines = [
        'TITLE "Warm"',
        "# Engine test fixture: red lifted, blue lowered (Tests/Data/Generate/MakeLutFixtures.py)",
        f"LUT_3D_SIZE {WARM_SIZE}",
        "DOMAIN_MIN 0.0 0.0 0.0",
        "DOMAIN_MAX 1.0 1.0 1.0",
        "",
    ]
    for blue in range(WARM_SIZE):
        for green in range(WARM_SIZE):
            for red in range(WARM_SIZE):
                values = (
                    Fraction(1, 10) + Fraction(9, 10) * Fraction(red, last),
                    Fraction(green, last),
                    Fraction(85, 100) * Fraction(blue, last),
                )
                lines.append(" ".join(format_value(value) for value in values))
    return ("\n".join(lines) + "\n").encode("ascii")


FIXTURES: dict[str, Callable[[], bytes]] = {
    "Warm.cube": make_warm_cube,
}


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the .cube colour-grading LUT fixtures in Tests/Data/Assets/Luts.",
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
    print(f"{'FAILED' if failed else 'passed'}: {len(FIXTURES)} LUT fixtures, {len(differences)} {verb}", file=out)
    if options.json:
        document = {"status": "failed" if failed else "passed", "check": options.check, "fixtures": sorted(FIXTURES),
                    "differences": differences, "errors": errors}
        print(json.dumps(document, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
