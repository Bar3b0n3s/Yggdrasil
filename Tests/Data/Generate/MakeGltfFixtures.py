#!/usr/bin/env python3
"""Generate the glTF importer fixtures in Tests/Data/Assets/Gltf (Roadmap M6, Docs/Decisions/0010-m6-decisions.md).

Every fixture is hand-built from the glTF 2.0 specification with the standard library only: the JSON is written with
sorted keys, two-space indentation and LF line endings, binary buffers are packed little-endian with struct, and PNG
images are encoded with zlib at a fixed compression level, so the output is byte-for-byte identical on every platform
and Python 3.10+. Nothing is downloaded and nothing is random.

The fixtures (paths relative to Tests/Data/Assets/Gltf):
  Box.gltf                     a unit cube, one node, one mesh, one primitive, POSITION/NORMAL/TEXCOORD_0, buffer as
                               a base64 data URI, one default material
  Box.glb                      the same cube as a binary glTF (JSON chunk plus BIN chunk)
  Textured.gltf                a cube with an external buffer (Textured.bin) and an external base color image
  Textured.bin                 (Textures/Checker.png): the dependency closure asset.import copies
  Textures/Checker.png         an 8 x 8 RGBA checker
  NormalMapped.gltf            a quad with a normal texture and a TANGENT attribute
  NormalMappedNoTangents.gltf  the same quad without TANGENT (tangents are generated with MikkTSpace)
  AlphaModes.gltf              three materials: OPAQUE, MASK (alphaCutoff 0.3) and BLEND
  DataUri.gltf                 buffer and image both as data URIs, no external file
  ParentEscape.gltf            a buffer URI "../Outside.bin" (rejected: escapes the source directory)
  AbsoluteUri.gltf             a buffer URI "/Models/Box.bin" (rejected: absolute)
  DriveLetterUri.gltf          a buffer URI "C:/Models/Box.bin" (rejected: absolute)
  HttpUri.gltf                 a buffer URI "http://example.com/Box.bin" (rejected: not a relative file)
  EncodedParentEscape.gltf     a buffer URI "%2e%2e/Outside.bin" (rejected: escapes once percent-decoded)
  EncodedBackslash.gltf        a buffer URI "..%5COutside.bin" (rejected: a decoded backslash)
  EncodedAbsolute.gltf         a buffer URI "%2FModels/Box.bin" (rejected: absolute once percent-decoded)
  EncodedUri.gltf              the cube with its buffer at the URI "Encoded%20Data/Box.bin", which names
  Encoded Data/Box.bin         "Encoded Data/Box.bin" once percent-decoded (accepted)
  TexCoord1Occlusion.gltf      an occlusion texture on TEXCOORD_1 (ASSET_UNSUPPORTED_UV_SET; UV set 0 is used)
  VertexColors.gltf            a primitive with COLOR_0 (ASSET_VERTEX_COLORS_IGNORED)
  DracoRequired.gltf           extensionsRequired ["KHR_draco_mesh_compression"] (refused with ASSET_IMPORT_FAILED)
  StandaloneTexture.gltf       a material whose base color is Textures/Shared.png, which a project also imports as a
  Textures/Shared.png          standalone texture asset (the material reuses that asset instead of a sub-asset)

Run from the repository root: `python Tests/Data/Generate/MakeGltfFixtures.py` writes the files; `--check` compares a
fresh generation with the committed files and writes nothing; `--json` prints a machine-readable report.

Exit codes: 0 success, 1 --check found a difference or a file could not be written, 2 usage error.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

OUTPUT_DIRECTORY = Path(__file__).resolve().parents[1] / "Assets" / "Gltf"


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate the glTF importer fixtures in Tests/Data/Assets/Gltf.")
    parser.add_argument("--check", action="store_true", help="compare with the committed files and write nothing")
    parser.add_argument("--json", action="store_true", help="print a machine-readable report on stdout")
    return parser.parse_args(arguments)


def generate(output: Path, check: bool) -> list[str]:
    """Writes (or, with `check`, compares) every fixture under `output`; returns the relative paths that differed."""
    raise NotImplementedError("contract stub: implemented by M6 stream C")


def main(arguments: list[str] | None = None) -> int:
    options = parse_arguments(arguments)
    differences = generate(OUTPUT_DIRECTORY, options.check)
    return 1 if differences else 0


if __name__ == "__main__":
    sys.exit(main())
