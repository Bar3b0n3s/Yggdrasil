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
  TexCoord1Occlusion.gltf      an occlusion texture on TEXCOORD_1 (ASSET_UNSUPPORTED_UV_SET; the slot is left empty)
  VertexColors.gltf            a primitive with COLOR_0 (ASSET_VERTEX_COLORS_IGNORED)
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
        make_vertex_colors,
        make_draco_required,
        make_standalone_texture,
        make_hierarchy,
        make_primitive_modes,
        make_skipped_content,
        make_materials,
        make_no_normals_no_uvs,
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
