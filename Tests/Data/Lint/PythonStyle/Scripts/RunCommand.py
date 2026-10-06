#!/usr/bin/env python3
"""Seeded defects for the Python step of Scripts/Lint.py."""

import subprocess
import sys


def run(command):
    try:
        return subprocess.run(command, shell=True, check=False).returncode
    except:
        return 1


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
