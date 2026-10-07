#!/usr/bin/env python3
"""Download the approved third-party assets from their official sources (Docs/Architecture.md §2.3, Roadmap M6).

Commands:
  hdri <id> --res 1k|2k|4k --dest <dir>
      A Poly Haven HDRI in Radiance HDR format. The file list comes from https://api.polyhaven.com/files/<id> and the
      file from dl.polyhaven.org; no other host is contacted, redirects to another host fail the download, and the
      downloaded bytes must match the md5 that the API lists. Written as <dest>/<id>_<res>.hdr.
  font --dest <dir>
      The pinned Inter release (Inter-Regular.ttf from the github.com/rsms/inter release archive), verified against
      its pinned SHA-256.
  defaults
      The committed engine resources (Docs/Decisions/0001-approvals.md): studio_small_09 and
      kloofendal_48d_partly_cloudy_puresky at 1k as Resources/Environments/Studio.hdr and Sky.hdr, and Inter-Regular.ttf
      as Resources/Fonts/Inter-Regular.ttf, each verified against the SHA-256 pinned in Resources/LICENSES.md.

Downloads need operator approval, which Roadmap M0 collects up front; the items this script fetches are the ones
recorded in Docs/Decisions/0001-approvals.md. Files are written to a temporary name and renamed into place only after
verification, so a failed download leaves nothing behind.

Exit codes: 0 success, 1 a download failed or did not verify, 2 usage error.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

POLY_HAVEN_API_HOST = "api.polyhaven.com"
POLY_HAVEN_DOWNLOAD_HOST = "dl.polyhaven.org"
HDRI_RESOLUTIONS = ("1k", "2k", "4k")


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Download the approved third-party assets from official sources.")
    parser.add_argument("--json", action="store_true", help="print a machine-readable report on stdout")
    commands = parser.add_subparsers(dest="command", required=True)
    hdri = commands.add_parser("hdri", help="a Poly Haven HDRI (md5-verified)")
    hdri.add_argument("id", help="the Poly Haven asset id, for example studio_small_09")
    hdri.add_argument("--res", choices=HDRI_RESOLUTIONS, default="1k", help="the resolution (default 1k)")
    hdri.add_argument("--dest", type=Path, required=True, help="the destination directory")
    font = commands.add_parser("font", help="the pinned Inter release (SHA-256-verified)")
    font.add_argument("--dest", type=Path, required=True, help="the destination directory")
    commands.add_parser("defaults", help="the committed engine resources under Resources/")
    return parser.parse_args(arguments)


def fetch_hdri(asset_id: str, resolution: str, destination: Path) -> Path:
    """Downloads one Poly Haven HDRI into `destination` and returns the written file."""
    raise NotImplementedError("contract stub: implemented by M6 stream F")


def fetch_font(destination: Path) -> Path:
    """Downloads the pinned Inter-Regular.ttf into `destination` and returns the written file."""
    raise NotImplementedError("contract stub: implemented by M6 stream F")


def fetch_defaults() -> list[Path]:
    """Downloads the committed engine resources into Resources/ and returns the written files."""
    raise NotImplementedError("contract stub: implemented by M6 stream F")


def main(arguments: list[str] | None = None) -> int:
    options = parse_arguments(arguments)
    if options.command == "hdri":
        fetch_hdri(options.id, options.res, options.dest)
    elif options.command == "font":
        fetch_font(options.dest)
    else:
        fetch_defaults()
    return 0


if __name__ == "__main__":
    sys.exit(main())
