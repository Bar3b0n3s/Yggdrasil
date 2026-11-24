#!/usr/bin/env python3
"""Generate the glTF importer fixtures in Tests/Data/Assets/Gltf (Roadmap M6, Docs/Decisions/0010-m6-decisions.md).

Every fixture is hand-built from the glTF 2.0 specification with the standard library only: the JSON is written with
sorted keys, two-space indentation and LF line endings, binary buffers are packed little-endian with struct, and PNG
images are written with stored (uncompressed) deflate blocks that this script frames itself, so no zlib build can
change a byte. The output is therefore byte-for-byte identical on every platform and Python 3.10+. Nothing is
downloaded and nothing is random.

The fixtures (paths relative to Tests/Data/Assets/Gltf). Unless noted, a mesh has POSITION, NORMAL and TEXCOORD_0 and
no TANGENT (the importer generates tangents with MikkTSpace), texture coordinates have their origin at the top left
(glTF), and buffers and images are base64 data URIs:
  Box.gltf                     a unit cube, one node, one mesh, one primitive, one material
  Box.glb                      the same cube as a binary glTF (JSON chunk plus BIN chunk), with its base colour image
                               embedded in the BIN chunk through a buffer view
  Textured.gltf                a cube with an external buffer (Textured.bin) and an external base colour image
  Textured.bin                 (Textures/Checker.png), the dependency closure asset.import copies; its mesh has two
  Textures/Checker.png         primitives (the sides with the textured material, the caps with an untextured one) and
                               Checker.png is an 8 x 8 RGBA checker
  NormalMapped.gltf            a quad with a normal texture and a TANGENT attribute (the quad's MikkTSpace tangents)
  NormalMappedNoTangents.gltf  the same quad without TANGENT (tangents are generated with MikkTSpace)
  AlphaModes.gltf              three materials: OPAQUE, MASK (alphaCutoff 0.25) and BLEND, one primitive each
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
  TexCoord1Occlusion.gltf      an occlusion texture on TEXCOORD_1, which the primitive has (the material's
                               OcclusionUVSet is UV1, M23)
  TexCoord2Occlusion.gltf      an occlusion texture on TEXCOORD_2, which the primitive has (ASSET_UNSUPPORTED_UV_SET;
                               the slot is left empty)
  VertexColors.gltf            a primitive with float VEC4 COLOR_0 (the mesh's attribute stream, M23)
  Attributes.gltf              one mesh of three quads (M23, MeshData version 2's attribute stream): the first with
                               TEXCOORD_1 as normalized unsigned bytes and COLOR_0 as normalized unsigned short VEC3,
                               its material's base colour and normal textures on TEXCOORD_1; the second with COLOR_0
                               as normalized unsigned byte VEC4 only; the third with neither (the stream's defaults)
  LateAttributes.gltf          one mesh of two quads (M23): the first with neither TEXCOORD_1 nor COLOR_0, so its
                               vertices take the stream's defaults once the second's are imported; the second with float
                               TEXCOORD_1 and float VEC4 COLOR_0 channels above 1 and below 0 (clamped into [0, 1])
  DracoRequired.gltf           extensionsRequired ["KHR_draco_mesh_compression"] (refused with ASSET_IMPORT_FAILED)
  StandaloneTexture.gltf       a material whose base color is Textures/Shared.png, which a project also imports as a
  Textures/Shared.png          standalone texture asset (the material reuses that asset instead of a sub-asset)
  Hierarchy.gltf               a node hierarchy in the second of two scenes ("scene": 1): translation, rotation and
                               scale nodes, an unnamed node given as a mirroring matrix with a child, one mesh used
                               twice, and a node that only the first scene lists
  PrimitiveModes.gltf          one mesh with a TRIANGLE_STRIP, a TRIANGLE_FAN (not indexed), a POINTS and a LINES
                               primitive, and a triangle list holding one degenerate triangle
  SkippedContent.gltf          a cube with a skin, an animation, a camera and a KHR_lights_punctual light
  Materials.gltf               every material property the importer maps: factors, KHR_materials_emissive_strength
                               (also in extensionsRequired), KHR_texture_transform, double-sided, and five texture
                               slots over three images (one image used as colour twice, one as linear data twice)
  NoNormalsNoUvs.gltf          a quad with POSITION only (normals generated, ASSET_TANGENTS_APPROXIMATED)
  Skinned.gltf                 M23 (GltfImporter version 3): a strip of two quads skinned to a two-joint chain under an
                               "Armature" node beside the skinned "Body", one morph target "Bulge" and one animation
                               "Wave" turning the second joint and keying the bulge's weight
  SkinnedRigs.gltf             M23: a rig under an intermediate node "Rig" (a static joint above the root-motion
                               joint "Hips"), a root-level joint "Tail" holding its own skinned mesh, an animation
                               "Walk" and an animation "Blink" shorter than a clip's shortest duration
  SharedSkins.gltf             M23: two skins over shared joints, each with its own mesh and inverse bind matrices (the
                               second mesh authored one unit off), merged into one Skeleton; a static use of the first
                               mesh; and an animation "Sway" over joints of both skins
  Replace/V1/Kit.gltf,         World Tree brief 08 (asset.import {replace: true}): two versions of one file name, each
  Replace/V1/Kit.bin,          with an external buffer and image. V1: the skinned strip "Strip" (Skinned.gltf's chain
  Replace/V1/Textures/         and animation "Wave") beside the quad "Panel", textured with Textures/Panel.png. V2: the
    Panel.png                  strip twice as wide, and "Cap", textured with Textures/Cap.png, instead of "Panel"; the
  Replace/V2/Kit.gltf,         skin and "Wave" kept. So a replace of V1 by V2 keeps mesh:0:Strip (changed), removes
  Replace/V2/Kit.bin,          mesh:1:Panel, adds mesh:1:Cap, overwrites Kit.bin and leaves Panel.png unreferenced
  Replace/V2/Textures/Cap.png
  Replace/Broken/Kit.gltf      a third version that parses but does not import (its POSITION accessor claims more
                               elements than its buffer view holds), which a replace refuses before writing anything
  LodTree.gltf                 M46 (GltfImporter version 4, levels of detail and _WIND): a tree "Tree" (a box trunk,
                               material "Bark", and a sphere crown, material "Leaves") whose MSFT_lod extension names
                               two level nodes outside the scene, "TreeMid" and "TreeFar" (coarser crowns, the same
                               two materials), with MSFT_screencoverage [0.4, 0.15, 0.05] in its extras; the crowns
                               carry _WIND (normalized unsigned bytes on level 0, floats on the levels: x and z rising
                               with height, y a phase from the vertex's x, w 0), the trunks none
  LodRock.gltf                 M46: sibling nodes "Rock_LOD0", "Rock_LOD1" and "Rock_LOD2", spheres of 128, 32 and 8
                               triangles (material "Stone"), which FromGltf groups into one mesh named "Rock"
  LodSphere.gltf               M46: one sphere of 512 triangles (material "Ball") sharing its vertices across the whole
                               surface, which Lods Generate simplifies
  LodMismatch.gltf             M46: "Crate_LOD0" (a box, material "Wood") with Crate_LOD1 (a smaller box, "Wood": kept),
                               Crate_LOD2 (two primitives: its submeshes differ), Crate_LOD3 (material "Metal": its
                               material differs) and Crate_LOD5 (a gap: Crate_LOD4 is missing), each dropped with
                               ASSET_LOD_INVALID; the validation fixture ASSET_LOD_INVALID imports it

Run from the repository root: `python Tests/Data/Generate/MakeGltfFixtures.py` writes the files (and removes files of
the output directory it does not generate); `--check` compares a fresh generation with the committed files and writes
nothing; `--json` prints a machine-readable report.

Exit codes: 0 success, 1 --check found a difference or a file could not be written, 2 usage error.
"""

from __future__ import annotations

import argparse
import base64
import json
import struct
import sys
import zlib
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Any

OUTPUT_DIRECTORY = Path(__file__).resolve().parents[1] / "Assets" / "Gltf"

# glTF 2.0 constants: accessor component types, buffer view targets and primitive modes.
FLOAT = 5126
UNSIGNED_BYTE = 5121
UNSIGNED_SHORT = 5123
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
MODE_POINTS = 0
MODE_LINES = 1
MODE_TRIANGLES = 4
MODE_TRIANGLE_STRIP = 5
MODE_TRIANGLE_FAN = 6

Vector2 = tuple[float, float]
Vector3 = tuple[float, float, float]
Vector4 = tuple[float, float, float, float]
Color = tuple[int, int, int, int]


class Geometry:
    """An indexed triangle list with per-vertex positions, normals and texture coordinates."""

    def __init__(
        self, positions: list[Vector3], normals: list[Vector3], texcoords: list[Vector2], indices: list[int]
    ) -> None:
        self.positions = positions
        self.normals = normals
        self.texcoords = texcoords
        self.indices = indices


# The six faces of the unit cube as (normal, right, up): right x up = normal, so the corners below are counter-clockwise
# seen from outside, and u grows along right while v grows against up (top-left origin).
CUBE_FACES: tuple[tuple[Vector3, Vector3, Vector3], ...] = (
    ((1.0, 0.0, 0.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0)),
    ((-1.0, 0.0, 0.0), (0.0, 0.0, 1.0), (0.0, 1.0, 0.0)),
    ((0.0, 0.0, 1.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)),
    ((0.0, 0.0, -1.0), (-1.0, 0.0, 0.0), (0.0, 1.0, 0.0)),
    ((0.0, 1.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, -1.0)),
    ((0.0, -1.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0)),
)


def add_face(geometry: Geometry, normal: Vector3, right: Vector3, up: Vector3, center: Vector3) -> None:
    """Appends a unit square: corners top-left, top-right, bottom-right, bottom-left; counter-clockwise triangles."""
    base = len(geometry.positions)
    corners = ((-0.5, 0.5, 0.0, 0.0), (0.5, 0.5, 1.0, 0.0), (0.5, -0.5, 1.0, 1.0), (-0.5, -0.5, 0.0, 1.0))
    for along_right, along_up, u, v in corners:
        position = tuple(center[axis] + right[axis] * along_right + up[axis] * along_up for axis in range(3))
        geometry.positions.append((position[0], position[1], position[2]))
        geometry.normals.append(normal)
        geometry.texcoords.append((u, v))
    geometry.indices.extend((base + 3, base + 2, base + 1, base + 3, base + 1, base))


def cube_geometry() -> Geometry:
    geometry = Geometry([], [], [], [])
    for normal, right, up in CUBE_FACES:
        add_face(geometry, normal, right, up, (normal[0] * 0.5, normal[1] * 0.5, normal[2] * 0.5))
    return geometry


def quad_geometry(center: Vector3 = (0.0, 0.0, 0.0)) -> Geometry:
    """A unit square in the XY plane facing +Z."""
    geometry = Geometry([], [], [], [])
    add_face(geometry, (0.0, 0.0, 1.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), center)
    return geometry


def subtract(a: Sequence[float], b: Sequence[float]) -> list[float]:
    return [a[axis] - b[axis] for axis in range(len(a))]


def dot(a: Sequence[float], b: Sequence[float]) -> float:
    return sum(a[axis] * b[axis] for axis in range(len(a)))


def normalize(vector: Sequence[float]) -> list[float]:
    length = dot(vector, vector) ** 0.5
    return [component / length for component in vector]


def mikktspace_tangents(geometry: Geometry) -> list[Vector4]:
    """Per-vertex tangents in the glTF convention, equal to MikkTSpace's for the planar, affinely mapped faces here.

    Each triangle's tangent is dP/du, made perpendicular to the normal; the sign w is +1 when the bitangent
    cross(normal, tangent) points towards decreasing v (up in the image, glTF's top-left origin) and -1 otherwise, which
    is MikkTSpace's sign for a bottom-left origin, negated.
    """
    sums: list[list[float]] = [[0.0, 0.0, 0.0] for _ in geometry.positions]
    signs: list[float] = [1.0 for _ in geometry.positions]
    for corner in range(0, len(geometry.indices), 3):
        a, b, c = geometry.indices[corner : corner + 3]
        edge1 = subtract(geometry.positions[b], geometry.positions[a])
        edge2 = subtract(geometry.positions[c], geometry.positions[a])
        du1, dv1 = subtract(geometry.texcoords[b], geometry.texcoords[a])
        du2, dv2 = subtract(geometry.texcoords[c], geometry.texcoords[a])
        area = du1 * dv2 - du2 * dv1
        tangent = [(edge1[axis] * dv2 - edge2[axis] * dv1) / area for axis in range(3)]
        for vertex in (a, b, c):
            sums[vertex] = [sums[vertex][axis] + tangent[axis] for axis in range(3)]
            signs[vertex] = 1.0 if area < 0.0 else -1.0
    tangents: list[Vector4] = []
    for vertex, total in enumerate(sums):
        normal = geometry.normals[vertex]
        projected = subtract(total, [component * dot(normal, total) for component in normal])
        x, y, z = normalize(projected)
        tangents.append((x, y, z, signs[vertex]))
    return tangents


def zlib_stored(data: bytes) -> bytes:
    """A zlib stream (RFC 1950) of stored deflate blocks (RFC 1951 section 3.2.4): identical with every zlib build."""
    blocks = bytearray(b"\x78\x01")
    offset = 0
    while True:
        chunk = data[offset : offset + 65535]
        offset += len(chunk)
        final = offset >= len(data)
        blocks += struct.pack("<BHH", 1 if final else 0, len(chunk), len(chunk) ^ 0xFFFF)
        blocks += chunk
        if final:
            break
    blocks += struct.pack(">I", zlib.adler32(data) & 0xFFFFFFFF)
    return bytes(blocks)


def encode_png(width: int, height: int, pixel: Callable[[int, int], Color]) -> bytes:
    """An 8-bit RGBA PNG whose texel (x, y) is pixel(x, y) -> (r, g, b, a)."""

    def chunk(kind: bytes, payload: bytes) -> bytes:
        checksum = zlib.crc32(kind + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)

    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(pixel(x, y))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    signature = b"\x89PNG\r\n\x1a\n"
    return signature + chunk(b"IHDR", header) + chunk(b"IDAT", zlib_stored(bytes(rows))) + chunk(b"IEND", b"")


def checker_png(size: int, first: Color, second: Color) -> bytes:
    return encode_png(size, size, lambda x, y: first if (x + y) % 2 == 0 else second)


def solid_png(size: int, color: Color) -> bytes:
    return encode_png(size, size, lambda x, y: color)


CHECKER_PNG = checker_png(8, (230, 230, 230, 255), (40, 40, 40, 255))
SHARED_PNG = checker_png(4, (200, 60, 60, 255), (60, 60, 200, 255))
FLAT_NORMAL_PNG = solid_png(4, (128, 128, 255, 255))
BASE_COLOR_PNG = checker_png(2, (255, 200, 0, 255), (0, 120, 255, 255))
OCCLUSION_PNG = solid_png(2, (180, 180, 180, 255))
METALLIC_ROUGHNESS_PNG = solid_png(2, (0, 128, 255, 255))


def data_uri(media_type: str, payload: bytes) -> str:
    return f"data:{media_type};base64,{base64.b64encode(payload).decode('ascii')}"


class GltfBuilder:
    """Accumulates one buffer, its views and accessors, and the other top-level arrays of a glTF document."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.buffer_views: list[dict[str, Any]] = []
        self.accessors: list[dict[str, Any]] = []
        self.images: list[dict[str, Any]] = []
        self.textures: list[dict[str, Any]] = []
        self.materials: list[dict[str, Any]] = []
        self.meshes: list[dict[str, Any]] = []
        self.nodes: list[dict[str, Any]] = []
        self.extra: dict[str, Any] = {}

    def add_view(self, payload: bytes, target: int | None) -> int:
        while len(self.data) % 4 != 0:
            self.data.append(0)
        view: dict[str, Any] = {"buffer": 0, "byteLength": len(payload), "byteOffset": len(self.data)}
        if target is not None:
            view["target"] = target
        self.data += payload
        self.buffer_views.append(view)
        return len(self.buffer_views) - 1

    def add_accessor(self, payload: bytes, accessor: dict[str, Any], target: int | None) -> int:
        accessor = dict(accessor)
        accessor["bufferView"] = self.add_view(payload, target)
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def add_floats(
        self, values: Sequence[Sequence[float]], kind: str, with_bounds: bool = False, target: int | None = ARRAY_BUFFER
    ) -> int:
        width = len(values[0])
        payload = b"".join(struct.pack(f"<{width}f", *value) for value in values)
        accessor: dict[str, Any] = {"componentType": FLOAT, "count": len(values), "type": kind}
        if with_bounds:
            rounded = [struct.unpack(f"<{width}f", struct.pack(f"<{width}f", *value)) for value in values]
            accessor["min"] = [min(value[axis] for value in rounded) for axis in range(width)]
            accessor["max"] = [max(value[axis] for value in rounded) for axis in range(width)]
        return self.add_accessor(payload, accessor, target)

    def add_normalized(self, values: Sequence[Sequence[int]], kind: str, component_type: int) -> int:
        """A normalized unsigned byte or short vertex accessor, its elements 4-byte aligned (glTF 2.0 3.6.2.4)."""
        width = len(values[0])
        code = "B" if component_type == UNSIGNED_BYTE else "H"
        element = struct.calcsize(f"<{width}{code}")
        stride = element + (-element % 4)
        payload = b"".join(struct.pack(f"<{width}{code}", *value) + b"\0" * (stride - element) for value in values)
        accessor = {"componentType": component_type, "count": len(values), "normalized": True, "type": kind}
        index = self.add_accessor(payload, accessor, ARRAY_BUFFER)
        self.buffer_views[self.accessors[index]["bufferView"]]["byteStride"] = stride
        return index

    def add_indices(self, indices: Sequence[int]) -> int:
        payload = b"".join(struct.pack("<H", index) for index in indices)
        accessor = {"componentType": UNSIGNED_SHORT, "count": len(indices), "type": "SCALAR"}
        return self.add_accessor(payload, accessor, ELEMENT_ARRAY_BUFFER)

    def add_attributes(
        self, geometry: Geometry, normals: bool = True, texcoords: bool = True, tangents: bool = False
    ) -> dict[str, int]:
        attributes = {"POSITION": self.add_floats(geometry.positions, "VEC3", with_bounds=True)}
        if normals:
            attributes["NORMAL"] = self.add_floats(geometry.normals, "VEC3")
        if tangents:
            attributes["TANGENT"] = self.add_floats(mikktspace_tangents(geometry), "VEC4")
        if texcoords:
            attributes["TEXCOORD_0"] = self.add_floats(geometry.texcoords, "VEC2")
        return attributes

    def add_primitive(
        self, geometry: Geometry, material: int | None = None, **attribute_options: bool
    ) -> dict[str, Any]:
        primitive: dict[str, Any] = {
            "attributes": self.add_attributes(geometry, **attribute_options),
            "indices": self.add_indices(geometry.indices),
            "mode": MODE_TRIANGLES,
        }
        if material is not None:
            primitive["material"] = material
        return primitive

    def add_mesh(self, name: str, primitives: list[dict[str, Any]]) -> int:
        self.meshes.append({"name": name, "primitives": primitives})
        return len(self.meshes) - 1

    def add_node(self, node: dict[str, Any]) -> int:
        self.nodes.append(node)
        return len(self.nodes) - 1

    def add_image(self, image: dict[str, Any]) -> int:
        self.images.append(image)
        self.textures.append({"source": len(self.images) - 1})
        return len(self.textures) - 1

    def add_material(self, material: dict[str, Any]) -> int:
        self.materials.append(material)
        return len(self.materials) - 1

    def document(self, buffer_uri: str | None, scene_nodes: list[int] | None = None) -> dict[str, Any]:
        """The glTF JSON. buffer_uri None leaves the buffer without a URI (the BIN chunk of a .glb)."""
        while len(self.data) % 4 != 0:
            self.data.append(0)
        buffer: dict[str, Any] = {"byteLength": len(self.data)}
        if buffer_uri is not None:
            buffer["uri"] = buffer_uri
        document: dict[str, Any] = {
            "asset": {"generator": "Tests/Data/Generate/MakeGltfFixtures.py", "version": "2.0"},
            "accessors": self.accessors,
            "bufferViews": self.buffer_views,
            "buffers": [buffer],
            "meshes": self.meshes,
            "nodes": self.nodes,
            "scene": 0,
            "scenes": [{"nodes": scene_nodes if scene_nodes is not None else list(range(len(self.nodes)))}],
        }
        for key, values in (("images", self.images), ("textures", self.textures), ("materials", self.materials)):
            if values:
                document[key] = values
        document.update(self.extra)
        return document

    def embedded_document(self, scene_nodes: list[int] | None = None) -> dict[str, Any]:
        """The document with its buffer as a data URI."""
        while len(self.data) % 4 != 0:
            self.data.append(0)
        return self.document(data_uri("application/octet-stream", bytes(self.data)), scene_nodes)


def gltf_text(document: dict[str, Any]) -> bytes:
    return (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("ascii")


def glb_bytes(document: dict[str, Any], binary: bytes) -> bytes:
    """A binary glTF (glTF 2.0 section 4.4): header, JSON chunk padded with spaces, BIN chunk padded with zeros."""
    json_chunk = json.dumps(document, separators=(",", ":"), sort_keys=True).encode("ascii")
    json_chunk += b" " * (-len(json_chunk) % 4)
    binary += b"\0" * (-len(binary) % 4)
    length = 12 + 8 + len(json_chunk) + 8 + len(binary)
    return (
        struct.pack("<4sII", b"glTF", 2, length)
        + struct.pack("<I4s", len(json_chunk), b"JSON")
        + json_chunk
        + struct.pack("<I4s", len(binary), b"BIN\0")
        + binary
    )


def plain_material(name: str, base_color: Vector4 = (0.8, 0.8, 0.8, 1.0)) -> dict[str, Any]:
    return {"name": name, "pbrMetallicRoughness": {"baseColorFactor": list(base_color), "metallicFactor": 0.0,
                                                   "roughnessFactor": 0.5}}


def textured_material(name: str, texture: int) -> dict[str, Any]:
    material = plain_material(name, (1.0, 1.0, 1.0, 1.0))
    material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": texture}
    return material


def cube_builder(material: dict[str, Any] | None = None, node_name: str = "Box") -> GltfBuilder:
    """A builder holding the cube mesh "Box" on one node, with `material` (a plain one when None)."""
    builder = GltfBuilder()
    material_index = builder.add_material(material if material is not None else plain_material("BoxMaterial"))
    mesh = builder.add_mesh("Box", [builder.add_primitive(cube_geometry(), material_index)])
    builder.add_node({"mesh": mesh, "name": node_name})
    return builder


def make_box() -> dict[str, bytes]:
    return {"Box.gltf": gltf_text(cube_builder().embedded_document())}


def make_box_glb() -> dict[str, bytes]:
    builder = GltfBuilder()
    builder.images.append({"bufferView": builder.add_view(CHECKER_PNG, None), "mimeType": "image/png"})
    builder.textures.append({"source": 0})
    material = builder.add_material(textured_material("BoxMaterial", 0))
    mesh = builder.add_mesh("Box", [builder.add_primitive(cube_geometry(), material)])
    builder.add_node({"mesh": mesh, "name": "Box"})
    document = builder.document(None)
    return {"Box.glb": glb_bytes(document, bytes(builder.data))}


def make_textured() -> dict[str, bytes]:
    builder = GltfBuilder()
    texture = builder.add_image({"uri": "Textures/Checker.png"})
    checker = builder.add_material(textured_material("Checker", texture))
    trim = builder.add_material(plain_material("Trim", (0.2, 0.2, 0.25, 1.0)))
    geometry = cube_geometry()
    attributes = builder.add_attributes(geometry)
    sides = builder.add_indices(geometry.indices[:24])
    caps = builder.add_indices(geometry.indices[24:])
    primitives = [
        {"attributes": attributes, "indices": sides, "material": checker, "mode": MODE_TRIANGLES},
        {"attributes": attributes, "indices": caps, "material": trim, "mode": MODE_TRIANGLES},
    ]
    mesh = builder.add_mesh("TexturedCube", primitives)
    builder.add_node({"mesh": mesh, "name": "TexturedCube"})
    document = builder.document("Textured.bin")
    return {
        "Textured.gltf": gltf_text(document),
        "Textured.bin": bytes(builder.data),
        "Textures/Checker.png": CHECKER_PNG,
    }


def normal_mapped(tangents: bool) -> bytes:
    builder = GltfBuilder()
    texture = builder.add_image({"uri": data_uri("image/png", FLAT_NORMAL_PNG)})
    material = plain_material("NormalMapped")
    material["normalTexture"] = {"index": texture, "scale": 1.0}
    material_index = builder.add_material(material)
    mesh = builder.add_mesh("Panel", [builder.add_primitive(quad_geometry(), material_index, tangents=tangents)])
    builder.add_node({"mesh": mesh, "name": "Panel"})
    return gltf_text(builder.embedded_document())


def make_normal_mapped() -> dict[str, bytes]:
    return {"NormalMapped.gltf": normal_mapped(True), "NormalMappedNoTangents.gltf": normal_mapped(False)}


def make_alpha_modes() -> dict[str, bytes]:
    builder = GltfBuilder()
    opaque = builder.add_material(plain_material("Opaque"))
    mask = plain_material("Mask")
    mask.update({"alphaCutoff": 0.25, "alphaMode": "MASK"})
    blend = plain_material("Blend", (0.8, 0.8, 0.8, 0.5))
    blend["alphaMode"] = "BLEND"
    materials = [opaque, builder.add_material(mask), builder.add_material(blend)]
    primitives = [builder.add_primitive(quad_geometry((float(slot) * 1.5, 0.0, 0.0)), material)
                  for slot, material in enumerate(materials)]
    mesh = builder.add_mesh("Panels", primitives)
    builder.add_node({"mesh": mesh, "name": "Panels"})
    return {"AlphaModes.gltf": gltf_text(builder.embedded_document())}


def make_data_uri() -> dict[str, bytes]:
    builder = GltfBuilder()
    texture = builder.add_image({"uri": data_uri("image/png", BASE_COLOR_PNG)})
    material = builder.add_material(textured_material("Embedded", texture))
    mesh = builder.add_mesh("Quad", [builder.add_primitive(quad_geometry(), material)])
    builder.add_node({"mesh": mesh, "name": "Quad"})
    return {"DataUri.gltf": gltf_text(builder.embedded_document())}


REJECTED_URIS = (
    ("ParentEscape.gltf", "../Outside.bin"),
    ("AbsoluteUri.gltf", "/Models/Box.bin"),
    ("DriveLetterUri.gltf", "C:/Models/Box.bin"),
    ("HttpUri.gltf", "http://example.com/Box.bin"),
    ("EncodedParentEscape.gltf", "%2e%2e/Outside.bin"),
    ("EncodedBackslash.gltf", "..%5COutside.bin"),
    ("EncodedAbsolute.gltf", "%2FModels/Box.bin"),
)


def make_uri_fixtures() -> dict[str, bytes]:
    files = {name: gltf_text(cube_builder().document(uri)) for name, uri in REJECTED_URIS}
    encoded = cube_builder()
    files["EncodedUri.gltf"] = gltf_text(encoded.document("Encoded%20Data/Box.bin"))
    files["Encoded Data/Box.bin"] = bytes(encoded.data)
    return files


def make_texcoord1_occlusion() -> dict[str, bytes]:
    builder = GltfBuilder()
    texture = builder.add_image({"uri": data_uri("image/png", OCCLUSION_PNG)})
    material = plain_material("Occluded")
    material["occlusionTexture"] = {"index": texture, "strength": 1.0, "texCoord": 1}
    material_index = builder.add_material(material)
    geometry = quad_geometry()
    primitive = builder.add_primitive(geometry, material_index)
    second_set = [(u * 0.5, v * 0.5) for u, v in geometry.texcoords]
    primitive["attributes"]["TEXCOORD_1"] = builder.add_floats(second_set, "VEC2")
    mesh = builder.add_mesh("Occluded", [primitive])
    builder.add_node({"mesh": mesh, "name": "Occluded"})
    return {"TexCoord1Occlusion.gltf": gltf_text(builder.embedded_document())}


def make_texcoord2_occlusion() -> dict[str, bytes]:
    builder = GltfBuilder()
    texture = builder.add_image({"uri": data_uri("image/png", OCCLUSION_PNG)})
    material = plain_material("Occluded")
    material["occlusionTexture"] = {"index": texture, "strength": 1.0, "texCoord": 2}
    material_index = builder.add_material(material)
    geometry = quad_geometry()
    primitive = builder.add_primitive(geometry, material_index)
    for name, scale in (("TEXCOORD_1", 0.5), ("TEXCOORD_2", 0.25)):
        scaled = [(u * scale, v * scale) for u, v in geometry.texcoords]
        primitive["attributes"][name] = builder.add_floats(scaled, "VEC2")
    mesh = builder.add_mesh("Occluded", [primitive])
    builder.add_node({"mesh": mesh, "name": "Occluded"})
    return {"TexCoord2Occlusion.gltf": gltf_text(builder.embedded_document())}


def make_attributes() -> dict[str, bytes]:
    builder = GltfBuilder()
    base_color = builder.add_image({"uri": data_uri("image/png", BASE_COLOR_PNG)})
    normal = builder.add_image({"uri": data_uri("image/png", FLAT_NORMAL_PNG)})
    painted = textured_material("Painted", base_color)
    painted["pbrMetallicRoughness"]["baseColorTexture"]["texCoord"] = 1
    painted["normalTexture"] = {"index": normal, "scale": 1.0, "texCoord": 1}
    plain = plain_material("Plain")
    materials = [builder.add_material(material) for material in (painted, plain, plain_material("Bare"))]
    primitives = []
    for slot, material in enumerate(materials):
        geometry = quad_geometry((float(slot) * 1.5, 0.0, 0.0))
        primitive = builder.add_primitive(geometry, material)
        if slot == 0:
            # The second UV set mirrors the first horizontally, in 255ths; the colours are 16-bit RGB.
            second_set = [(255 - round(u * 255), round(v * 255)) for u, v in geometry.texcoords]
            primitive["attributes"]["TEXCOORD_1"] = builder.add_normalized(second_set, "VEC2", UNSIGNED_BYTE)
            colors = [(65535, 0, 0), (0, 65535, 0), (0, 0, 65535), (32768, 32768, 32768)]
            primitive["attributes"]["COLOR_0"] = builder.add_normalized(colors, "VEC3", UNSIGNED_SHORT)
        elif slot == 1:
            colors = [(255, 0, 0, 255), (0, 255, 0, 128), (0, 0, 255, 64), (255, 255, 255, 0)]
            primitive["attributes"]["COLOR_0"] = builder.add_normalized(colors, "VEC4", UNSIGNED_BYTE)
        primitives.append(primitive)
    mesh = builder.add_mesh("Painted", primitives)
    builder.add_node({"mesh": mesh, "name": "Painted"})
    return {"Attributes.gltf": gltf_text(builder.embedded_document())}


def make_late_attributes() -> dict[str, bytes]:
    builder = GltfBuilder()
    materials = [builder.add_material(plain_material(name)) for name in ("Bare", "Late")]
    primitives = []
    for slot, material in enumerate(materials):
        geometry = quad_geometry((float(slot) * 1.5, 0.0, 0.0))
        primitive = builder.add_primitive(geometry, material)
        if slot == 1:
            second_set = [(u * 0.5, v * 0.5) for u, v in geometry.texcoords]
            primitive["attributes"]["TEXCOORD_1"] = builder.add_floats(second_set, "VEC2")
            colors = [(1.5, -0.25, 0.5, 2.0), (-1.0, 0.25, 1.0001, -0.5), (0.0, 1.0, 0.75, 1.0), (4.0, 4.0, 4.0, 0.5)]
            primitive["attributes"]["COLOR_0"] = builder.add_floats(colors, "VEC4")
        primitives.append(primitive)
    mesh = builder.add_mesh("Late", primitives)
    builder.add_node({"mesh": mesh, "name": "Late"})
    return {"LateAttributes.gltf": gltf_text(builder.embedded_document())}


def make_vertex_colors() -> dict[str, bytes]:
    builder = GltfBuilder()
    material = builder.add_material(plain_material("Painted"))
    geometry = quad_geometry()
    primitive = builder.add_primitive(geometry, material)
    colors = [(1.0, 0.0, 0.0, 1.0), (0.0, 1.0, 0.0, 1.0), (0.0, 0.0, 1.0, 1.0), (1.0, 1.0, 1.0, 1.0)]
    primitive["attributes"]["COLOR_0"] = builder.add_floats(colors, "VEC4")
    mesh = builder.add_mesh("Painted", [primitive])
    builder.add_node({"mesh": mesh, "name": "Painted"})
    return {"VertexColors.gltf": gltf_text(builder.embedded_document())}


def make_draco_required() -> dict[str, bytes]:
    builder = GltfBuilder()
    compressed = builder.add_view(bytes(range(16)), None)
    builder.accessors.append({"componentType": FLOAT, "count": 3, "max": [1.0, 1.0, 0.0], "min": [0.0, 0.0, 0.0],
                              "type": "VEC3"})
    draco = {"KHR_draco_mesh_compression": {"attributes": {"POSITION": 0}, "bufferView": compressed}}
    primitive = {"attributes": {"POSITION": 0}, "extensions": draco, "mode": MODE_TRIANGLES}
    mesh = builder.add_mesh("Compressed", [primitive])
    builder.add_node({"mesh": mesh, "name": "Compressed"})
    builder.extra = {"extensionsRequired": ["KHR_draco_mesh_compression"],
                     "extensionsUsed": ["KHR_draco_mesh_compression"]}
    return {"DracoRequired.gltf": gltf_text(builder.embedded_document())}


def make_standalone_texture() -> dict[str, bytes]:
    builder = GltfBuilder()
    texture = builder.add_image({"uri": "Textures/Shared.png"})
    material = builder.add_material(textured_material("Shared", texture))
    mesh = builder.add_mesh("Box", [builder.add_primitive(cube_geometry(), material)])
    builder.add_node({"mesh": mesh, "name": "Box"})
    return {"StandaloneTexture.gltf": gltf_text(builder.embedded_document()), "Textures/Shared.png": SHARED_PNG}


def make_hierarchy() -> dict[str, bytes]:
    builder = GltfBuilder()
    material = builder.add_material(plain_material("Paint"))
    mesh = builder.add_mesh("Cube", [builder.add_primitive(cube_geometry(), material)])
    half_sqrt2 = 0.7071067811865476
    builder.add_node({"children": [1, 2], "name": "Base", "translation": [0.0, 1.0, 0.0]})
    quarter_turn = [0.0, half_sqrt2, 0.0, half_sqrt2]
    builder.add_node({"mesh": mesh, "name": "Left", "rotation": quarter_turn, "scale": [2.0, 2.0, 2.0],
                      "translation": [-2.0, 0.0, 0.0]})
    # Translation (3, 0, 0) and a mirror of Z, as a column-major matrix.
    mirror = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 3.0, 0.0, 0.0, 1.0]
    builder.add_node({"children": [3], "matrix": mirror, "mesh": mesh})
    builder.add_node({"name": "Leaf", "translation": [0.0, 0.0, 1.0]})
    builder.add_node({"mesh": mesh, "name": "Unused"})
    document = builder.embedded_document()
    document["scenes"] = [{"name": "Other", "nodes": [4]}, {"name": "Main", "nodes": [0]}]
    document["scene"] = 1
    return {"Hierarchy.gltf": gltf_text(document)}


def make_primitive_modes() -> dict[str, bytes]:
    builder = GltfBuilder()
    material = builder.add_material(plain_material("Modes"))
    normal = (0.0, 0.0, 1.0)

    def attributes(positions: list[Vector3], texcoords: list[Vector2]) -> dict[str, int]:
        return {
            "NORMAL": builder.add_floats([normal] * len(positions), "VEC3"),
            "POSITION": builder.add_floats(positions, "VEC3", with_bounds=True),
            "TEXCOORD_0": builder.add_floats(texcoords, "VEC2"),
        }

    # Strip order: bottom-left, bottom-right, top-left, top-right. Fan order: counter-clockwise around the quad.
    strip = attributes([(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (1.0, 1.0, 0.0)],
                       [(0.0, 1.0), (1.0, 1.0), (0.0, 0.0), (1.0, 0.0)])
    fan = attributes([(2.0, 0.0, 0.0), (3.0, 0.0, 0.0), (3.0, 1.0, 0.0), (2.0, 1.0, 0.0)],
                     [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)])
    points = attributes([(4.0, 0.0, 0.0), (5.0, 0.0, 0.0)], [(0.0, 0.0), (1.0, 0.0)])
    lines = attributes([(6.0, 0.0, 0.0), (7.0, 0.0, 0.0)], [(0.0, 0.0), (1.0, 0.0)])
    listed = attributes([(8.0, 0.0, 0.0), (9.0, 0.0, 0.0), (8.0, 1.0, 0.0)], [(0.0, 1.0), (1.0, 1.0), (0.0, 0.0)])
    primitives = [
        {"attributes": strip, "indices": builder.add_indices([0, 1, 2, 3]), "material": material,
         "mode": MODE_TRIANGLE_STRIP},
        {"attributes": fan, "material": material, "mode": MODE_TRIANGLE_FAN},
        {"attributes": points, "material": material, "mode": MODE_POINTS},
        {"attributes": lines, "material": material, "mode": MODE_LINES},
        # The second triangle repeats an index: it is degenerate and dropped.
        {"attributes": listed, "indices": builder.add_indices([0, 1, 2, 0, 0, 1]), "material": material,
         "mode": MODE_TRIANGLES},
    ]
    mesh = builder.add_mesh("Modes", primitives)
    builder.add_node({"mesh": mesh, "name": "Modes"})
    return {"PrimitiveModes.gltf": gltf_text(builder.embedded_document())}


def make_skipped_content() -> dict[str, bytes]:
    builder = cube_builder()
    times = builder.add_floats([(0.0,), (1.0,)], "SCALAR", with_bounds=True, target=None)
    translations = builder.add_floats([(0.0, 0.0, 0.0), (0.0, 1.0, 0.0)], "VEC3", target=None)
    builder.add_node({"name": "Joint"})
    builder.add_node({"camera": 0, "name": "Camera", "translation": [0.0, 0.0, 5.0]})
    builder.add_node({"extensions": {"KHR_lights_punctual": {"light": 0}}, "name": "Lamp"})
    builder.extra = {
        "animations": [{"channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}],
                        "samplers": [{"input": times, "interpolation": "LINEAR", "output": translations}]}],
        "cameras": [{"perspective": {"yfov": 0.8, "znear": 0.1}, "type": "perspective"}],
        "extensions": {"KHR_lights_punctual": {"lights": [{"type": "point"}]}},
        "extensionsUsed": ["KHR_lights_punctual"],
        "skins": [{"joints": [1]}],
    }
    return {"SkippedContent.gltf": gltf_text(builder.embedded_document())}


def make_materials() -> dict[str, bytes]:
    builder = GltfBuilder()
    color = builder.add_image({"uri": data_uri("image/png", BASE_COLOR_PNG)})
    normal = builder.add_image({"uri": data_uri("image/png", FLAT_NORMAL_PNG)})
    data = builder.add_image({"uri": data_uri("image/png", METALLIC_ROUGHNESS_PNG)})
    transform = {"KHR_texture_transform": {"offset": [0.25, 0.5], "scale": [2.0, 3.0]}}
    material = {
        "alphaCutoff": 0.75,
        "alphaMode": "MASK",
        "doubleSided": True,
        "emissiveFactor": [1.0, 0.5, 0.25],
        "emissiveTexture": {"index": color},
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4.0}},
        "name": "Everything",
        "normalTexture": {"index": normal, "scale": 0.5},
        "occlusionTexture": {"index": data, "strength": 0.75},
        "pbrMetallicRoughness": {
            "baseColorFactor": [0.5, 0.25, 1.0, 1.0],
            "baseColorTexture": {"extensions": transform, "index": color},
            "metallicFactor": 0.25,
            "metallicRoughnessTexture": {"index": data},
            "roughnessFactor": 0.75,
        },
    }
    material_index = builder.add_material(material)
    mesh = builder.add_mesh("Quad", [builder.add_primitive(quad_geometry(), material_index)])
    builder.add_node({"mesh": mesh, "name": "Quad"})
    extensions = ["KHR_materials_emissive_strength", "KHR_texture_transform"]
    builder.extra = {"extensionsRequired": ["KHR_materials_emissive_strength"], "extensionsUsed": extensions}
    return {"Materials.gltf": gltf_text(builder.embedded_document())}


def make_no_normals_no_uvs() -> dict[str, bytes]:
    builder = GltfBuilder()
    primitive = builder.add_primitive(quad_geometry(), None, normals=False, texcoords=False)
    mesh = builder.add_mesh("Bare", [primitive])
    builder.add_node({"mesh": mesh, "name": "Bare"})
    return {"NoNormalsNoUvs.gltf": gltf_text(builder.embedded_document())}


# M23 (C23.3, GltfImporter version 3): skins, animations and morph targets. Matrices are 4 x 4 lists of rows; glTF
# stores them column-major (matrix_floats).
Matrix = list[list[float]]


def identity_matrix() -> Matrix:
    return [[1.0 if row == column else 0.0 for column in range(4)] for row in range(4)]


def multiply(a: Matrix, b: Matrix) -> Matrix:
    return [[sum(a[row][k] * b[k][column] for k in range(4)) for column in range(4)] for row in range(4)]


def translation_matrix(offset: Vector3) -> Matrix:
    matrix = identity_matrix()
    for axis in range(3):
        matrix[axis][3] = offset[axis]
    return matrix


def rotation_matrix(rotation: Vector4) -> Matrix:
    """The rotation of the unit quaternion (x, y, z, w)."""
    x, y, z, w = rotation
    return [
        [1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w), 0.0],
        [2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w), 0.0],
        [2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y), 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]


def rigid_inverse(matrix: Matrix) -> Matrix:
    """The inverse of a rotation followed by a translation: the transposed rotation, and the translation undone."""
    inverse = identity_matrix()
    for row in range(3):
        for column in range(3):
            inverse[row][column] = matrix[column][row]
    for row in range(3):
        inverse[row][3] = -sum(inverse[row][k] * matrix[k][3] for k in range(3))
    return inverse


def matrix_floats(matrix: Matrix) -> tuple[float, ...]:
    """The 16 floats of a glTF MAT4, column-major; -0.0 written as 0.0 so the bytes do not depend on rounding signs."""
    return tuple(matrix[row][column] + 0.0 for column in range(4) for row in range(4))


# The sine and cosine of half of each angle the fixtures turn by, as exact literals: the doubles the committed fixtures
# were generated with. The C runtime's sin and cos, which Python's math module calls, are not guaranteed bit-identical
# across machines or runtime updates, so the committed bytes never depend on them (the engine bans them on its
# simulation path for the same reason; W23.3's review).
HALF_TURN_SINES_AND_COSINES: dict[float, tuple[float, float]] = {
    10.0: (float.fromhex("0x1.64fd6b8c28102p-4"), float.fromhex("0x1.fe0d3b41815a2p-1")),
    45.0: (float.fromhex("0x1.87de2a6aea963p-2"), float.fromhex("0x1.d906bcf328d46p-1")),
    90.0: (float.fromhex("0x1.6a09e667f3bcdp-1"), float.fromhex("0x1.6a09e667f3bcdp-1")),
}
assert all(abs(sine * sine + cosine * cosine - 1.0) <= 4.0e-16 for sine, cosine in HALF_TURN_SINES_AND_COSINES.values())


def axis_rotation(axis: Vector3, degrees: float) -> Vector4:
    """The unit quaternion (x, y, z, w) turning `degrees` (an angle of HALF_TURN_SINES_AND_COSINES) about the unit
    `axis`."""
    sine, cosine = HALF_TURN_SINES_AND_COSINES[degrees]
    return (axis[0] * sine, axis[1] * sine, axis[2] * sine, cosine)


def node_matrix(node: dict[str, Any]) -> Matrix:
    """A node's local transform (translation and rotation; the fixtures' joints have no scale)."""
    translation = node.get("translation", [0.0, 0.0, 0.0])
    rotation = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    return multiply(translation_matrix((translation[0], translation[1], translation[2])),
                    rotation_matrix((rotation[0], rotation[1], rotation[2], rotation[3])))


def world_matrices(nodes: list[dict[str, Any]]) -> list[Matrix]:
    """Every node's world transform (parents listed before their children in the fixtures)."""
    parents: dict[int, int] = {}
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            parents[child] = index
    worlds: list[Matrix] = []
    for index, node in enumerate(nodes):
        local = node_matrix(node)
        worlds.append(multiply(worlds[parents[index]], local) if index in parents else local)
    return worlds


def strip_geometry(left: float, bottom: float, width: float, row_height: float) -> Geometry:
    """A vertical strip of two quads in the XY plane facing +Z: three rows of two vertices (bottom row first), each quad
    as two counter-clockwise triangles."""
    geometry = Geometry([], [], [], [])
    for row in range(3):
        for column in range(2):
            geometry.positions.append((left + width * column, bottom + row_height * row, 0.0))
            geometry.normals.append((0.0, 0.0, 1.0))
            geometry.texcoords.append((float(column), 1.0 - row / 2.0))
    for quad in range(2):
        base = quad * 2
        geometry.indices.extend((base, base + 1, base + 3, base, base + 3, base + 2))
    return geometry


def add_joints(builder: GltfBuilder, joints: Sequence[Sequence[int]]) -> int:
    """A JOINTS_0 accessor: VEC4 unsigned bytes (4 bytes an element, so aligned without padding)."""
    payload = b"".join(struct.pack("<4B", *joint) for joint in joints)
    accessor = {"componentType": UNSIGNED_BYTE, "count": len(joints), "type": "VEC4"}
    return builder.add_accessor(payload, accessor, ARRAY_BUFFER)


def add_skinned_primitive(
    builder: GltfBuilder, geometry: Geometry, material: int, rows: Sequence[tuple[tuple[int, int], Vector2]]
) -> dict[str, Any]:
    """A primitive of a strip whose row r's two vertices take JOINTS_0 (rows[r][0], 0, 0) and WEIGHTS_0
    (rows[r][1], 0, 0)."""
    primitive = builder.add_primitive(geometry, material)
    count = len(geometry.positions)
    joints = [(rows[vertex // 2][0][0], rows[vertex // 2][0][1], 0, 0) for vertex in range(count)]
    weights = [(rows[vertex // 2][1][0], rows[vertex // 2][1][1], 0.0, 0.0) for vertex in range(count)]
    primitive["attributes"]["JOINTS_0"] = add_joints(builder, joints)
    primitive["attributes"]["WEIGHTS_0"] = builder.add_floats(weights, "VEC4")
    return primitive


def add_inverse_binds(builder: GltfBuilder, matrices: Sequence[Matrix]) -> int:
    return builder.add_floats([matrix_floats(matrix) for matrix in matrices], "MAT4", target=None)


def add_sampler(
    builder: GltfBuilder,
    samplers: list[dict[str, Any]],
    times: Sequence[float],
    values: Sequence[Sequence[float]],
    kind: str,
) -> int:
    """A LINEAR sampler keying `values` (of glTF accessor type `kind`) at `times`; returns its index in `samplers`."""
    times_accessor = builder.add_floats([(time,) for time in times], "SCALAR", with_bounds=True, target=None)
    values_accessor = builder.add_floats(values, kind, target=None)
    samplers.append({"input": times_accessor, "interpolation": "LINEAR", "output": values_accessor})
    return len(samplers) - 1


def make_skinned() -> dict[str, bytes]:
    """Skinned.gltf: a strip of two quads (rows at y 0, 1 and 2) skinned to the two-joint chain Joint0 -> Joint1 under
    "Armature", beside the skinned "Body" (so Body's Animator goes on Armature), with one morph target "Bulge" (the
    middle row pushed 0.25 along +Z, the outer rows' normals tilted to follow) and one animation "Wave" that turns
    Joint1 45 degrees about Z and back and keys the bulge's weight 0, 1, 0 over one second."""
    builder = GltfBuilder()
    material = builder.add_material(plain_material("StripMaterial", (0.8, 0.5, 0.3, 1.0)))
    geometry = strip_geometry(-0.5, 0.0, 1.0, 1.0)
    rows = (((0, 0), (1.0, 0.0)), ((0, 1), (0.5, 0.5)), ((1, 0), (1.0, 0.0)))
    primitive = add_skinned_primitive(builder, geometry, material, rows)
    bulge = 0.25
    length = (1.0 + bulge * bulge) ** 0.5
    row_normals = ((0.0, -bulge / length, 1.0 / length), (0.0, 0.0, 1.0), (0.0, bulge / length, 1.0 / length))
    position_deltas = [(0.0, 0.0, bulge if vertex // 2 == 1 else 0.0) for vertex in range(6)]
    normal_deltas = [
        tuple(row_normals[vertex // 2][axis] - (0.0, 0.0, 1.0)[axis] for axis in range(3)) for vertex in range(6)
    ]
    primitive["targets"] = [{"NORMAL": builder.add_floats(normal_deltas, "VEC3"),
                             "POSITION": builder.add_floats(position_deltas, "VEC3", with_bounds=True)}]
    mesh = builder.add_mesh("Strip", [primitive])
    builder.meshes[mesh]["weights"] = [0.0]
    builder.meshes[mesh]["extras"] = {"targetNames": ["Bulge"]}

    builder.add_node({"children": [1, 3], "name": "Armature"})
    builder.add_node({"children": [2], "name": "Joint0"})
    builder.add_node({"name": "Joint1", "translation": [0.0, 1.0, 0.0]})
    builder.add_node({"mesh": mesh, "name": "Body", "skin": 0})
    worlds = world_matrices(builder.nodes)
    inverse_binds = add_inverse_binds(builder, [rigid_inverse(worlds[1]), rigid_inverse(worlds[2])])

    samplers: list[dict[str, Any]] = []
    turned = axis_rotation((0.0, 0.0, 1.0), 45.0)
    rest = (0.0, 0.0, 0.0, 1.0)
    rotation = add_sampler(builder, samplers, (0.0, 0.5, 1.0), (rest, turned, rest), "VEC4")
    weights = add_sampler(builder, samplers, (0.0, 0.5, 1.0), ((0.0,), (1.0,), (0.0,)), "SCALAR")
    builder.extra = {
        "animations": [{"channels": [{"sampler": rotation, "target": {"node": 2, "path": "rotation"}},
                                     {"sampler": weights, "target": {"node": 3, "path": "weights"}}],
                        "name": "Wave", "samplers": samplers}],
        "skins": [{"inverseBindMatrices": inverse_binds, "joints": [1, 2]}],
    }
    return {"Skinned.gltf": gltf_text(builder.embedded_document(scene_nodes=[0]))}


def make_skinned_rigs() -> dict[str, bytes]:
    """SkinnedRigs.gltf, two rigs for the Animator placement rules. "Character" holds "Rig" (turned 90 degrees about Y
    and lifted 1, not a joint) above skin 0's root joint "Hips" and its child "Spine", beside the skinned "Body"; the
    root-level joint "Tail" (skin 1, at x 2) holds its own skinned "TailMesh". Animation "Walk" moves the hips 1 along
    +Z over one second; "Blink" turns the spine over 0.0001 s, shorter than a clip's shortest duration."""
    builder = GltfBuilder()
    material = builder.add_material(plain_material("RigMaterial"))
    body = add_skinned_primitive(builder, strip_geometry(-0.25, 1.5, 0.5, 0.5), material,
                                 (((0, 0), (1.0, 0.0)), ((0, 1), (0.5, 0.5)), ((1, 0), (1.0, 0.0))))
    body_mesh = builder.add_mesh("BodyMesh", [body])
    tail = add_skinned_primitive(builder, strip_geometry(1.75, 0.0, 0.5, 0.25), material,
                                 (((0, 0), (1.0, 0.0)), ((0, 0), (1.0, 0.0)), ((0, 0), (1.0, 0.0))))
    tail_mesh = builder.add_mesh("TailMesh", [tail])

    builder.add_node({"children": [1, 4], "name": "Character"})
    builder.add_node({"children": [2], "name": "Rig", "rotation": list(axis_rotation((0.0, 1.0, 0.0), 90.0)),
                      "translation": [0.0, 1.0, 0.0]})
    builder.add_node({"children": [3], "name": "Hips", "translation": [0.0, 0.5, 0.0]})
    builder.add_node({"name": "Spine", "translation": [0.0, 0.5, 0.0]})
    builder.add_node({"mesh": body_mesh, "name": "Body", "skin": 0})
    builder.add_node({"children": [6], "name": "Tail", "translation": [2.0, 0.0, 0.0]})
    builder.add_node({"mesh": tail_mesh, "name": "TailMesh", "skin": 1})
    worlds = world_matrices(builder.nodes)
    rig_binds = add_inverse_binds(builder, [rigid_inverse(worlds[2]), rigid_inverse(worlds[3])])
    tail_binds = add_inverse_binds(builder, [rigid_inverse(worlds[5])])

    walk: list[dict[str, Any]] = []
    stride = add_sampler(builder, walk, (0.0, 1.0), ((0.0, 0.5, 0.0), (0.0, 0.5, 1.0)), "VEC3")
    blink: list[dict[str, Any]] = []
    nodded = axis_rotation((1.0, 0.0, 0.0), 10.0)
    nod = add_sampler(builder, blink, (0.0, 0.0001), ((0.0, 0.0, 0.0, 1.0), nodded), "VEC4")
    builder.extra = {
        "animations": [
            {"channels": [{"sampler": stride, "target": {"node": 2, "path": "translation"}}], "name": "Walk",
             "samplers": walk},
            {"channels": [{"sampler": nod, "target": {"node": 3, "path": "rotation"}}], "name": "Blink",
             "samplers": blink},
        ],
        "skins": [{"inverseBindMatrices": rig_binds, "joints": [2, 3]},
                  {"inverseBindMatrices": tail_binds, "joints": [5]}],
    }
    return {"SkinnedRigs.gltf": gltf_text(builder.embedded_document(scene_nodes=[0, 5]))}


def make_shared_skins() -> dict[str, bytes]:
    """SharedSkins.gltf (the merge rule of GltfImporter version 3): the chain Root -> Upper -> Tip under "Armature",
    with two skins over shared joints, as an exporter gives each skinned mesh its own skin: "Lower" draws LowerMesh
    (rows at y 0, 0.5 and 1) with skin 0 over Root and Upper; "Top" draws TopMesh with skin 1 over Upper and Tip, its
    vertices authored one unit along -X of where they show (rows at y 1, 1.5 and 2, x from -1.5 to -0.5), which skin 1's
    inverse bind matrices carry back (each the inverse of its joint's world transform times a translation of +1 along
    X). Both skins resolve to Armature, so they import as one Skeleton. "Statue" draws LowerMesh without a skin, at
    x 3. Animation "Sway" turns Root and Tip 10 degrees about Z over one second."""
    builder = GltfBuilder()
    material = builder.add_material(plain_material("SharedMaterial"))
    lower = add_skinned_primitive(builder, strip_geometry(-0.5, 0.0, 1.0, 0.5), material,
                                  (((0, 0), (1.0, 0.0)), ((0, 1), (0.5, 0.5)), ((1, 0), (1.0, 0.0))))
    lower_mesh = builder.add_mesh("LowerMesh", [lower])
    top = add_skinned_primitive(builder, strip_geometry(-1.5, 1.0, 1.0, 0.5), material,
                                (((0, 0), (1.0, 0.0)), ((0, 1), (0.5, 0.5)), ((1, 0), (1.0, 0.0))))
    top_mesh = builder.add_mesh("TopMesh", [top])

    builder.add_node({"children": [1, 4, 5, 6], "name": "Armature"})
    builder.add_node({"children": [2], "name": "Root"})
    builder.add_node({"children": [3], "name": "Upper", "translation": [0.0, 1.0, 0.0]})
    builder.add_node({"name": "Tip", "translation": [0.0, 1.0, 0.0]})
    builder.add_node({"mesh": lower_mesh, "name": "Lower", "skin": 0})
    builder.add_node({"mesh": top_mesh, "name": "Top", "skin": 1})
    builder.add_node({"mesh": lower_mesh, "name": "Statue", "translation": [3.0, 0.0, 0.0]})
    worlds = world_matrices(builder.nodes)
    authored = translation_matrix((1.0, 0.0, 0.0))
    lower_binds = add_inverse_binds(builder, [rigid_inverse(worlds[1]), rigid_inverse(worlds[2])])
    top_binds = add_inverse_binds(
        builder, [multiply(rigid_inverse(worlds[2]), authored), multiply(rigid_inverse(worlds[3]), authored)]
    )

    samplers: list[dict[str, Any]] = []
    swayed = axis_rotation((0.0, 0.0, 1.0), 10.0)
    root = add_sampler(builder, samplers, (0.0, 1.0), ((0.0, 0.0, 0.0, 1.0), swayed), "VEC4")
    tip = add_sampler(builder, samplers, (0.0, 1.0), ((0.0, 0.0, 0.0, 1.0), swayed), "VEC4")
    builder.extra = {
        "animations": [{"channels": [{"sampler": root, "target": {"node": 1, "path": "rotation"}},
                                     {"sampler": tip, "target": {"node": 3, "path": "rotation"}}],
                        "name": "Sway", "samplers": samplers}],
        "skins": [{"inverseBindMatrices": lower_binds, "joints": [1, 2]},
                  {"inverseBindMatrices": top_binds, "joints": [2, 3]}],
    }
    return {"SharedSkins.gltf": gltf_text(builder.embedded_document(scene_nodes=[0]))}


PANEL_PNG = checker_png(2, (40, 160, 90, 255), (220, 220, 220, 255))
CAP_PNG = checker_png(2, (160, 40, 90, 255), (20, 20, 20, 255))


def replace_version(strip_width: float, second: str, image: bytes) -> dict[str, bytes]:
    """One version of Replace/<V>/Kit.gltf (World Tree brief 08's asset.import {replace: true}): the skinned strip
    "Strip" (mesh 0, `strip_width` wide, rows at y 0, 1 and 2) with Skinned.gltf's two-joint chain under "Armature" and
    its animation "Wave" turning Joint1, beside the mesh `second` (mesh 1, a quad at x 2 with the material
    "<second>Material", whose base colour is Textures/<second>.png, `image`), every buffer in the external Kit.bin."""
    builder = GltfBuilder()
    strip_material = builder.add_material(plain_material("StripMaterial", (0.8, 0.5, 0.3, 1.0)))
    texture = builder.add_image({"uri": f"Textures/{second}.png"})
    second_material = builder.add_material(textured_material(f"{second}Material", texture))
    rows = (((0, 0), (1.0, 0.0)), ((0, 1), (0.5, 0.5)), ((1, 0), (1.0, 0.0)))
    geometry = strip_geometry(-strip_width / 2.0, 0.0, strip_width, 1.0)
    strip = add_skinned_primitive(builder, geometry, strip_material, rows)
    strip_mesh = builder.add_mesh("Strip", [strip])
    second_mesh = builder.add_mesh(second, [builder.add_primitive(quad_geometry((2.0, 0.5, 0.0)), second_material)])

    builder.add_node({"children": [1, 3], "name": "Armature"})
    builder.add_node({"children": [2], "name": "Joint0"})
    builder.add_node({"name": "Joint1", "translation": [0.0, 1.0, 0.0]})
    builder.add_node({"mesh": strip_mesh, "name": "Body", "skin": 0})
    builder.add_node({"mesh": second_mesh, "name": second})
    worlds = world_matrices(builder.nodes)
    inverse_binds = add_inverse_binds(builder, [rigid_inverse(worlds[1]), rigid_inverse(worlds[2])])

    samplers: list[dict[str, Any]] = []
    turned = axis_rotation((0.0, 0.0, 1.0), 45.0)
    rest = (0.0, 0.0, 0.0, 1.0)
    rotation = add_sampler(builder, samplers, (0.0, 0.5, 1.0), (rest, turned, rest), "VEC4")
    builder.extra = {
        "animations": [{"channels": [{"sampler": rotation, "target": {"node": 2, "path": "rotation"}}], "name": "Wave",
                        "samplers": samplers}],
        "skins": [{"inverseBindMatrices": inverse_binds, "joints": [1, 2]}],
    }
    document = builder.document("Kit.bin", scene_nodes=[0, 4])
    return {"Kit.gltf": gltf_text(document), "Kit.bin": bytes(builder.data), f"Textures/{second}.png": image}


def broken_replace_version() -> bytes:
    """Replace/Broken/Kit.gltf: one triangle whose POSITION accessor claims six elements of a buffer view that holds
    three, so the file parses (and lists no external file) but its import fails with ImportFailed naming the
    accessor."""
    builder = GltfBuilder()
    positions = builder.add_floats([(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)], "VEC3", with_bounds=True)
    builder.accessors[positions]["count"] = 6
    mesh = builder.add_mesh("Strip", [{"attributes": {"POSITION": positions}, "mode": MODE_TRIANGLES}])
    builder.add_node({"mesh": mesh, "name": "Body"})
    return gltf_text(builder.embedded_document())


def make_replace() -> dict[str, bytes]:
    """Replace/V1 and Replace/V2, the two versions of Kit.gltf that asset.import {replace: true} swaps: V2's strip is
    twice as wide (mesh:0:Strip changed), its mesh "Panel" and material "PanelMaterial" (mesh 1, material 1) became
    "Cap" and "CapMaterial" (removed and added), its image Textures/Panel.png became Textures/Cap.png (Panel.png
    unreferenced; the sub-asset texture:0 kept with Cap.png's texels), Kit.bin holds other bytes, and the skin and the
    animation "Wave" are kept."""
    files: dict[str, bytes] = {}
    for version, (width, second, image) in (("V1", (1.0, "Panel", PANEL_PNG)), ("V2", (2.0, "Cap", CAP_PNG))):
        for name, content in replace_version(width, second, image).items():
            files[f"Replace/{version}/{name}"] = content
    files["Replace/Broken/Kit.gltf"] = broken_replace_version()
    return files


# M46 (GltfImporter version 4): spheres subdivided from an octahedron, every point normalized with sqrt (correctly
# rounded), so the bytes are the same on every platform; one vertex per point (no seam), so the surface is closed.
OCTAHEDRON_POINTS: tuple[Vector3, ...] = (
    (1.0, 0.0, 0.0), (-1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, -1.0, 0.0), (0.0, 0.0, 1.0), (0.0, 0.0, -1.0),
)
OCTAHEDRON_FACES: tuple[tuple[int, int, int], ...] = (
    (4, 0, 2), (0, 5, 2), (5, 1, 2), (1, 4, 2), (0, 4, 3), (5, 0, 3), (1, 5, 3), (4, 1, 3),
)


def unit(vector: Sequence[float]) -> Vector3:
    length = (vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]) ** 0.5
    return (vector[0] / length, vector[1] / length, vector[2] / length)


def sphere_geometry(subdivisions: int, radius: float, center: Vector3) -> Geometry:
    """An octahedron subdivided `subdivisions` times (8 x 4^n counter-clockwise triangles), its points on the unit
    sphere scaled by `radius` about `center`; normals point outward, and u, v project x and y (no seam)."""
    points: list[Vector3] = list(OCTAHEDRON_POINTS)
    faces: list[tuple[int, int, int]] = list(OCTAHEDRON_FACES)
    for _ in range(subdivisions):
        midpoints: dict[tuple[int, int], int] = {}

        def midpoint(a: int, b: int) -> int:
            key = (min(a, b), max(a, b))
            if key not in midpoints:
                points.append(unit([(points[a][axis] + points[b][axis]) * 0.5 for axis in range(3)]))
                midpoints[key] = len(points) - 1
            return midpoints[key]

        subdivided: list[tuple[int, int, int]] = []
        for a, b, c in faces:
            ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
            subdivided += [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
        faces = subdivided
    positions = [(center[0] + point[0] * radius, center[1] + point[1] * radius, center[2] + point[2] * radius)
                 for point in points]
    texcoords = [(0.5 + 0.5 * point[0], 0.5 - 0.5 * point[1]) for point in points]
    return Geometry(positions, list(points), texcoords, [index for face in faces for index in face])


def box_geometry(size: Vector3, center: Vector3) -> Geometry:
    """The unit cube scaled by `size` about `center` (its normals unchanged: the scale is along the axes)."""
    cube = cube_geometry()
    positions = [(center[0] + x * size[0], center[1] + y * size[1], center[2] + z * size[2])
                 for x, y, z in cube.positions]
    return Geometry(positions, cube.normals, cube.texcoords, cube.indices)


def tree_wind(geometry: Geometry) -> list[Vector4]:
    """The crowns' _WIND: x (branch sway) and z (flutter) rising with height above the trunk's top at y = 1.6, y a phase
    from the vertex's x, w (bend mask) 0."""
    weights = []
    for x, y, _ in geometry.positions:
        height = min(max((y - 1.6) / 2.4, 0.0), 1.0)
        weights.append((height, min(max(0.5 + 0.4 * x, 0.0), 1.0), height, 0.0))
    return weights


def make_lod_tree() -> dict[str, bytes]:
    builder = GltfBuilder()
    bark = builder.add_material(plain_material("Bark", (0.35, 0.22, 0.12, 1.0)))
    leaves = builder.add_material(plain_material("Leaves", (0.2, 0.55, 0.2, 1.0)))
    meshes = []
    for level, (name, subdivisions) in enumerate((("Tree", 2), ("TreeMid", 1), ("TreeFar", 0))):
        trunk = builder.add_primitive(box_geometry((0.3, 2.0, 0.3), (0.0, 1.0, 0.0)), bark)
        crown_geometry = sphere_geometry(subdivisions, 1.2, (0.0, 2.8, 0.0))
        crown = builder.add_primitive(crown_geometry, leaves)
        wind = tree_wind(crown_geometry)
        if level == 0:
            crown["attributes"]["_WIND"] = builder.add_normalized(
                [tuple(round(value * 255) for value in weights) for weights in wind], "VEC4", UNSIGNED_BYTE)
        else:
            crown["attributes"]["_WIND"] = builder.add_floats(wind, "VEC4")
        meshes.append(builder.add_mesh(name, [trunk, crown]))
    builder.add_node({"mesh": meshes[0], "name": "Tree", "extensions": {"MSFT_lod": {"ids": [1, 2]}},
                      "extras": {"MSFT_screencoverage": [0.4, 0.15, 0.05]}})
    builder.add_node({"mesh": meshes[1], "name": "TreeMid"})
    builder.add_node({"mesh": meshes[2], "name": "TreeFar"})
    builder.extra["extensionsUsed"] = ["MSFT_lod"]
    return {"LodTree.gltf": gltf_text(builder.embedded_document(scene_nodes=[0]))}


def make_lod_rock() -> dict[str, bytes]:
    builder = GltfBuilder()
    stone = builder.add_material(plain_material("Stone", (0.5, 0.48, 0.45, 1.0)))
    for level, subdivisions in enumerate((2, 1, 0)):
        sphere = sphere_geometry(subdivisions, 1.0, (0.0, 0.8, 0.0))
        mesh = builder.add_mesh(f"Rock{level}", [builder.add_primitive(sphere, stone)])
        builder.add_node({"mesh": mesh, "name": f"Rock_LOD{level}"})
    return {"LodRock.gltf": gltf_text(builder.embedded_document())}


def make_lod_sphere() -> dict[str, bytes]:
    builder = GltfBuilder()
    ball = builder.add_material(plain_material("Ball", (0.8, 0.3, 0.2, 1.0)))
    mesh = builder.add_mesh("Sphere", [builder.add_primitive(sphere_geometry(3, 1.0, (0.0, 1.0, 0.0)), ball)])
    builder.add_node({"mesh": mesh, "name": "Sphere"})
    return {"LodSphere.gltf": gltf_text(builder.embedded_document())}


def make_lod_mismatch() -> dict[str, bytes]:
    builder = GltfBuilder()
    wood = builder.add_material(plain_material("Wood", (0.6, 0.4, 0.2, 1.0)))
    metal = builder.add_material(plain_material("Metal", (0.7, 0.7, 0.75, 1.0)))
    levels = (
        ("Crate_LOD0", [(wood, 1.0)]),
        ("Crate_LOD1", [(wood, 0.9)]),
        ("Crate_LOD2", [(wood, 0.8), (wood, 0.4)]),
        ("Crate_LOD3", [(metal, 0.7)]),
        ("Crate_LOD5", [(wood, 0.6)]),
    )
    for name, primitives in levels:
        boxes = [(box_geometry((size, size, size), (0.0, size * 0.5, 0.0)), material) for material, size in primitives]
        mesh = builder.add_mesh(name, [builder.add_primitive(box, material) for box, material in boxes])
        builder.add_node({"mesh": mesh, "name": name})
    return {"LodMismatch.gltf": gltf_text(builder.embedded_document())}


def build_fixtures() -> dict[str, bytes]:
    """Every fixture, keyed by its '/'-separated path relative to the output directory."""
    fixtures: dict[str, bytes] = {}
    for make in (
        make_box,
        make_box_glb,
        make_textured,
        make_normal_mapped,
        make_alpha_modes,
        make_data_uri,
        make_uri_fixtures,
        make_texcoord1_occlusion,
        make_texcoord2_occlusion,
        make_vertex_colors,
        make_attributes,
        make_late_attributes,
        make_draco_required,
        make_standalone_texture,
        make_hierarchy,
        make_primitive_modes,
        make_skipped_content,
        make_materials,
        make_no_normals_no_uvs,
        make_skinned,
        make_skinned_rigs,
        make_shared_skins,
        make_replace,
        make_lod_tree,
        make_lod_rock,
        make_lod_sphere,
        make_lod_mismatch,
    ):
        for name, content in make().items():
            if name in fixtures:
                raise ValueError(f"fixture '{name}' is generated twice")
            fixtures[name] = content
    return dict(sorted(fixtures.items()))


def existing_files(output: Path) -> list[str]:
    if not output.is_dir():
        return []
    return sorted(path.relative_to(output).as_posix() for path in output.rglob("*") if path.is_file())


def generate(output: Path, check: bool) -> list[str]:
    """Writes (or, with `check`, compares) every fixture under `output`; returns the relative paths that differed."""
    fixtures = build_fixtures()
    differences: list[str] = []
    for name, content in fixtures.items():
        path = output / name
        current = path.read_bytes() if path.is_file() else None
        if current == content:
            continue
        differences.append(name)
        if not check:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
    for name in existing_files(output):
        if name in fixtures:
            continue
        differences.append(name)
        if not check:
            (output / name).unlink()
    return sorted(differences)


def parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate the glTF importer fixtures in Tests/Data/Assets/Gltf.")
    parser.add_argument("--check", action="store_true", help="compare with the committed files and write nothing")
    parser.add_argument("--json", action="store_true", help="print a machine-readable report on stdout")
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    options = parse_arguments(arguments)
    try:
        differences = generate(OUTPUT_DIRECTORY, options.check)
    except OSError as error:
        if options.json:
            print(json.dumps({"check": options.check, "error": str(error), "passed": False}))
        else:
            print(f"error: {error}", file=sys.stderr)
        return 1

    failed = options.check and bool(differences)
    if options.json:
        print(json.dumps({"check": options.check, "differences": differences, "passed": not failed}, sort_keys=True))
    elif options.check:
        for name in differences:
            print(f"differs: {name}")
        print(f"{'FAILED' if failed else 'passed'}: {len(differences)} fixture(s) differ from a fresh generation")
    else:
        for name in differences:
            print(f"updated: {name}")
        print(f"passed: {len(differences)} fixture file(s) written or removed in {OUTPUT_DIRECTORY}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
