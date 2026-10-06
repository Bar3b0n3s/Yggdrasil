#!/usr/bin/env python3
"""Seeded defect for Scripts/Lint.py: the product name in a Python string."""

import sys

# Control: a comment may name it (Yggdrasil); comments are not code.
TITLE = "Yggdrasil release tools"


def main() -> int:
    print(TITLE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
