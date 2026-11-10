#!/usr/bin/env python3
"""Generate the project-validation fixtures and the settings observation project (ADR 0022 decisions 11 and 12).

Two output roots, each owned whole by this generator (a file it does not write is reported as extra and, without
--check, removed):
- Tests/Data/Validation/<Name>/: `Project/` (a complete mini project: the .eproj, a .luaurc when it has scripts,
  Assets/) and `Expected.json` ({"Format": "ValidationFixture", "Version": 1, "Scope": "Project", "Codes",
  "TestComponents", "Expand", "NonCanonical"}). <Name> is a code of ProjectValidator::GetCodes, whose Codes contain it,
  or <CODE>.<Variant>, a further fixture of that code for another path that reports it (<CODE>.2D: the 2D world's
  composition, Docs/Decisions/0032-m21-contract.md decision 12; <CODE>.Tilemap: a tilemap's palette sheets, its decision
  23), or Clean, the baseline (a primary camera with the audio listener, and a sun) that reports nothing. Every fixture
  starts from that baseline, so incidental codes stay out; Codes lists every code the project reports, sorted.
- Tests/Data/SettingsObservation/Project/: the files Tests/Data/Project/AllSettings.eproj names (Assets/Icon.png, the
  start, build and suite scenes, the suite script and its replay) and a module whose Assets.Load path is computed (for
  Export.PruneUnreferenced), opened by "ProjectSettings: every AllSettings.eproj value is observed by its consumer"
  together with that .eproj.

Special cases (also in Tests/Data/Validation/README.md, which this generator writes):
- COMPONENT_MISSING_REQUIREMENT and ENTITY_DANGLING_REFERENCE use the test components ValidatorTarget (an EntityRef
  field) and ValidatorFollower (requires ValidatorTarget), registered when TestComponents is true: every built-in
  Requires is Transform, which every entity has, and no user-facing built-in component has an EntityRef field.
- PATH_CASE_MISMATCH pairs Grey.png with grey.png.meta. The two names differ in more than letter case, so git checks
  both out on every file system, and the scan reports the meta whose name matches its source only ignoring case,
  whether the file system is case-sensitive or not.
- PHYSICS_LIMIT_EXCEEDED, PHYSICS_LIMIT_EXCEEDED.2D and RENDER_LIGHT_LIMIT_EXCEEDED name an entity in Expand
  ({"Path", "Entity", "Count"}): the test repeats it until the scene holds Count of it, so no multi-megabyte scene is
  committed.
- NonCanonical lists project-relative paths that are deliberately not canonical JSON (ASSET_IMPORT_FAILED's unreadable
  .meta); the authored-files test exempts exactly those.
- ASSET_MISSING.Tilemap and ASSET_TYPE_MISMATCH.Tilemap name a sprite sheet from a tilemap's palette. Sheets have no
  placeholder, so the asset manager logs nothing for them and only the validator reports them; the test declares no
  Error entry for those two codes there.
- ANIMATION_NO_ANIMATOR and ANIMATION_SKELETON_MISMATCH reference the meshes and skeletons of generated skinned glTF
  files (GltfImporter version 3) by their sub-asset handles, Hash64(the .meta's handle, the sub-asset key), so their
  .meta files fix the handles.

Scenes, metas and project files are written in the canonical JsonWriter form; images are PNGs of stored deflate blocks,
so no zlib build changes their bytes. The glTF diagnostics fixtures and Truncated.png are byte copies of
Tests/Data/Assets files. Run from the repository root: without arguments it writes the files, `--check` compares and
writes nothing, `--json` prints a machine-readable report.

Exit codes: 0 fixtures generated or verified, 1 stale or extra files or an I/O failure, 2 usage error.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import json
import math
import struct
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / "Tests" / "Data"
VALIDATION = DATA / "Validation"
OBSERVATION = DATA / "SettingsObservation" / "Project"
ASSETS = DATA / "Assets"

PROJECT_FILE = "Validation.eproj"
MAIN_SCENE = "Assets/Scenes/Main.scene"
MAIN_SCENE_HANDLE = "5ca1e00000000001"
CUBE_MESH = "0000000000000101"
PLANE_MESH = "0000000000000103"

# The registry's canonical component order (BuiltinComponents, then the validator test components).
COMPONENT_ORDER = [
    "ID", "Name", "Tags", "Relationship", "Transform", "Prefab", "PrefabLink", "MeshRenderer", "Camera",
    "DirectionalLight", "PointLight", "SpotLight", "Environment", "PostProcess", "Text", "RigidBody", "BoxCollider",
    "SphereCollider", "CapsuleCollider", "MeshCollider", "CharacterController", "AudioSource", "AudioListener",
    "Script", "SpriteRenderer", "SpriteAnimator", "PixelPerfectCamera", "Light2D", "TilemapRenderer", "RigidBody2D",
    "BoxCollider2D", "CircleCollider2D", "PolygonCollider2D", "TilemapCollider", "Animator", "SkinnedMesh",
    "ValidatorTarget", "ValidatorFollower",
]

# The registered version of each component above 1 ("ComponentVersions"); every other component is at version 1.
COMPONENT_VERSIONS = {"Prefab": 2, "PointLight": 2, "Script": 2}

# ---------------------------------------------------------------------------------------------------------------------
# Canonical JSON (the JsonWriter form of Architecture §6: tabs, LF, scalar arrays on one line, final newline)
# ---------------------------------------------------------------------------------------------------------------------


def float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def format_number(value: float | int) -> str:
    """Integers as written; a float as the shortest decimal that reads back as the same float32 (JsonWriter's form)."""
    if isinstance(value, bool):
        raise TypeError("booleans are not numbers")
    if isinstance(value, int):
        return str(value)
    if not math.isfinite(value):
        raise ValueError("non-finite number")
    single = float32(value)
    if single == int(single):
        return str(int(single))
    for digits in range(1, 10):
        text = f"{single:.{digits}g}"
        if float32(float(text)) == single:
            if "e" in text:
                raise ValueError(f"{text}: fixture floats are written without an exponent")
            return text
    raise ValueError(f"{value} has no short float32 form")


def format_scalar(value: Any) -> str:
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    return format_number(value)


def write_json(value: Any, indent: int = 0) -> str:
    pad = "\t" * indent
    inner = "\t" * (indent + 1)
    if isinstance(value, dict):
        if not value:
            return "{}"
        members = [f"{inner}{json.dumps(key, ensure_ascii=False)}: {write_json(item, indent + 1)}"
                   for key, item in value.items()]
        return "{\n" + ",\n".join(members) + "\n" + pad + "}"
    if isinstance(value, list):
        if not value:
            return "[]"
        if all(not isinstance(item, (dict, list)) for item in value):
            return "[" + ", ".join(format_scalar(item) for item in value) + "]"
        return "[\n" + ",\n".join(inner + write_json(item, indent + 1) for item in value) + "\n" + pad + "]"
    return format_scalar(value)


def canonical(value: Any) -> str:
    return write_json(value) + "\n"


# ---------------------------------------------------------------------------------------------------------------------
# PNG (stored deflate blocks)
# ---------------------------------------------------------------------------------------------------------------------


def adler32(data: bytes) -> int:
    first, second = 1, 0
    for byte in data:
        first = (first + byte) % 65521
        second = (second + first) % 65521
    return (second << 16) | first


def zlib_stored(data: bytes) -> bytes:
    out = bytearray(b"\x78\x01")
    blocks = [data[index:index + 65535] for index in range(0, len(data), 65535)] or [b""]
    for index, block in enumerate(blocks):
        out.append(1 if index == len(blocks) - 1 else 0)
        out += struct.pack("<HH", len(block), len(block) ^ 0xFFFF)
        out += block
    out += struct.pack(">I", adler32(data))
    return bytes(out)


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", binascii.crc32(kind + payload) & 0xFFFFFFFF))


def checker_png(size: int) -> bytes:
    """A size x size 1-bit greyscale checkerboard of 32-pixel squares."""
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        row = 0
        bits = 0
        for x in range(size):
            row = (row << 1) | ((x // 32 + y // 32) % 2)
            bits += 1
            if bits == 8:
                rows.append(row)
                row = 0
                bits = 0
    header = struct.pack(">IIBBBBB", size, size, 1, 0, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib_stored(bytes(rows)))
            + png_chunk(b"IEND", b""))


_XXH_MASK = (1 << 64) - 1
_XXH_PRIMES = (0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5)


def hash64(seed: int, key: str) -> str:
    """Hash64(seed, key) of Engine/Core/Hash.h: the XXH64 of the key's UTF-8 bytes with `seed`, as a sub-asset's handle
    (Hash64(source handle, sub-asset key), Architecture section 7.1), in 16 lowercase hexadecimal digits."""
    prime1, prime2, prime3, prime4, prime5 = _XXH_PRIMES

    def rotate(value: int, count: int) -> int:
        value &= _XXH_MASK
        return ((value << count) | (value >> (64 - count))) & _XXH_MASK

    def round_lane(accumulator: int, lane: int) -> int:
        return (rotate(accumulator + lane * prime2, 31) * prime1) & _XXH_MASK

    data = key.encode("utf-8")
    offset = 0
    if len(data) >= 32:
        lanes = [(seed + prime1 + prime2) & _XXH_MASK, (seed + prime2) & _XXH_MASK, seed & _XXH_MASK,
                 (seed - prime1) & _XXH_MASK]
        while offset + 32 <= len(data):
            for index, word in enumerate(struct.unpack_from("<4Q", data, offset)):
                lanes[index] = round_lane(lanes[index], word)
            offset += 32
        result = sum(rotate(value, shift) for value, shift in zip(lanes, (1, 7, 12, 18))) & _XXH_MASK
        for value in lanes:
            result = ((result ^ round_lane(0, value)) * prime1 + prime4) & _XXH_MASK
    else:
        result = (seed + prime5) & _XXH_MASK
    result = (result + len(data)) & _XXH_MASK
    while offset + 8 <= len(data):
        result ^= round_lane(0, struct.unpack_from("<Q", data, offset)[0])
        result = (rotate(result, 27) * prime1 + prime4) & _XXH_MASK
        offset += 8
    if offset + 4 <= len(data):
        result ^= struct.unpack_from("<I", data, offset)[0] * prime1
        result = (rotate(result, 23) * prime2 + prime3) & _XXH_MASK
        offset += 4
    for byte in data[offset:]:
        result ^= byte * prime5
        result = (rotate(result, 11) * prime1) & _XXH_MASK
    result = ((result ^ (result >> 33)) * prime2) & _XXH_MASK
    result = ((result ^ (result >> 29)) * prime3) & _XXH_MASK
    return f"{result ^ (result >> 32):016x}"


def skinned_gltf(mesh: str, skin: str, joints: list[str]) -> str:
    """A glTF 2.0 file with one skinned triangle (the mesh `mesh` on its own node, every vertex on one joint) and the
    skin `skin` of `joints`, a chain from a root node, each one unit above its parent, with their inverse bind matrices.
    Its buffer is a base64 data URI; it has normals and tangents, so nothing is generated or approximated."""
    count = len(joints)
    positions = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)]
    payload = b"".join(struct.pack("<3f", *position) for position in positions)
    payload += struct.pack("<9f", *([0.0, 0.0, 1.0] * 3))
    payload += struct.pack("<12f", *([1.0, 0.0, 0.0, 1.0] * 3))
    payload += bytes(sum(([min(vertex, count - 1), 0, 0, 0] for vertex in range(3)), []))
    payload += struct.pack("<12f", *([1.0, 0.0, 0.0, 0.0] * 3))
    payload += struct.pack("<3H", 0, 1, 2) + bytes(2)
    for index in range(count):
        payload += struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -float(index), 0, 1)
    views = [(0, 36, 34962), (36, 36, 34962), (72, 48, 34962), (120, 12, 34962), (132, 48, 34962), (180, 6, 34963),
             (188, 64 * count, None)]
    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
        {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
        {"bufferView": 3, "componentType": 5121, "count": 3, "type": "VEC4"},
        {"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC4"},
        {"bufferView": 5, "componentType": 5123, "count": 3, "type": "SCALAR"},
        {"bufferView": 6, "componentType": 5126, "count": count, "type": "MAT4"},
    ]
    nodes: list[dict[str, Any]] = []
    for index, name in enumerate(joints):
        node: dict[str, Any] = {"name": name}
        if index > 0:
            node["translation"] = [0, 1, 0]
        if index + 1 < count:
            node["children"] = [index + 1]
        nodes.append(node)
    nodes.append({"name": mesh, "mesh": 0, "skin": 0})
    document = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0, count]}],
        "nodes": nodes,
        "skins": [{"name": skin, "joints": list(range(count)), "inverseBindMatrices": 6}],
        "meshes": [{"name": mesh, "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TANGENT": 2, "JOINTS_0": 3, "WEIGHTS_0": 4}, "indices": 5}]}],
        "accessors": accessors,
        "bufferViews": [dict({"buffer": 0, "byteOffset": offset, "byteLength": length},
                             **({"target": target} if target is not None else {})) for offset, length, target in views],
        "buffers": [{"byteLength": len(payload),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(payload).decode("ascii")}],
    }
    return json.dumps(document, indent="\t") + "\n"


GLTF_SETTINGS = {"Scale": 1, "GenerateMissingTangents": False, "ImportMaterials": True, "MergeMeshes": False}


# ---------------------------------------------------------------------------------------------------------------------
# Documents
# ---------------------------------------------------------------------------------------------------------------------


def default_project(name: str) -> dict[str, Any]:
    """The canonical default ProjectSettings document (Projects/FeatureTest/Scaffold/Baseline's shape)."""
    return {
        "Format": "Project", "Version": 1, "Name": name, "StartScene": "",
        "Window": {"Title": "Game", "Width": 1600, "Height": 900, "VSync": True, "Fullscreen": False,
                   "Resizable": True},
        "Simulation": {"FixedHz": 60, "MaxStepsPerFrame": 5, "Seed": 0, "MaxEntities": 65536},
        "Physics": {"Gravity": [0, -9.81, 0], "Layers": ["Default"], "Collisions": [["Default", "Default"]]},
        "Input": {"Actions": {}},
        "Rendering": {"ShadowMapSize": 2048, "SsaoHalfResolution": False},
        "Scripting": {"MemoryLimitMB": 256, "CallbackBudgetMs": 1000, "PauseOnError": True,
                      "BlockPlayOnTypeErrors": False},
        "Export": {"BuildScenes": [], "Exclude": ["Assets/Tests/**"], "Icon": "", "Version": "1.0.0", "Company": "",
                   "PruneUnreferenced": False},
        "Testing": {"Suites": [], "Replays": [], "CaseTimeoutTicks": 600, "SuiteTickLimit": 36000,
                    "CoverageGates": False},
        "SortingLayers": {"Layers": ["Default"]},
        "Physics2D": {"Gravity": [0, -9.81]},
    }


def transform(x: float = 0, y: float = 0, z: float = 0, scale: list[float] | None = None) -> dict[str, Any]:
    return {"Translation": [x, y, z], "Rotation": [0, 0, 0, 1], "Scale": scale or [1, 1, 1]}


def entity(identifier: str, name: str, components: dict[str, Any], parent: str | None = None) -> dict[str, Any]:
    ordered = {key: components[key] for key in COMPONENT_ORDER if key in components}
    unknown = set(components) - set(ordered)
    if unknown:
        raise ValueError(f"unknown components {sorted(unknown)}")
    return {"ID": identifier, "Name": name, "Parent": parent, "Active": True, "Tags": [], "Components": ordered}


def scene(name: str, entities: list[dict[str, Any]], seed: int = 0) -> dict[str, Any]:
    present = {key for item in entities for key in item["Components"]}
    versions = {key: COMPONENT_VERSIONS.get(key, 1) for key in COMPONENT_ORDER if key in present}
    return {"Format": "Scene", "Version": 1, "Name": name, "Seed": seed, "ComponentVersions": versions,
            "Entities": entities}


def prefab(name: str, root: str, entities: list[dict[str, Any]]) -> dict[str, Any]:
    present = {key for item in entities for key in item["Components"]}
    versions = {key: COMPONENT_VERSIONS.get(key, 1) for key in COMPONENT_ORDER if key in present}
    return {"Format": "Prefab", "Version": 1, "Name": name, "Root": root, "ComponentVersions": versions,
            "Entities": entities}


def meta(handle: str, kind: str, importer: str, settings: dict[str, Any] | None = None) -> dict[str, Any]:
    return {"Format": "AssetMeta", "Version": 1, "Handle": handle, "Type": kind, "Importer": importer,
            "ImporterVersion": 1, "Settings": settings, "SubAssets": []}


TEXTURE_SETTINGS = {"Usage": "Color", "GenerateMips": True}


def sprite_sheet(frames: list[tuple[str, int, int]], clips: list[tuple[str, list[tuple[int, int]]]],
                 size: int = 8, texture: str | None = None) -> dict[str, Any]:
    """A .spritesheet of `texture` (a handle, None for none): `size` x `size` frames (name, x, y) and looping clips
    (name, [(frame, ticks)])."""
    return {
        "Format": "SpriteSheet", "Version": 1, "Texture": texture,
        "Frames": [{"Name": name, "Rect": {"X": x, "Y": y, "Width": size, "Height": size}, "Pivot": [0.5, 0.5],
                    "Borders": {"Left": 0, "Top": 0, "Right": 0, "Bottom": 0}} for name, x, y in frames],
        "Clips": [{"Name": name, "Frames": [{"Frame": frame, "Ticks": ticks} for frame, ticks in steps], "Loop": True}
                  for name, steps in clips],
    }


def tilemap(palette: list[tuple[str | None, int, bool]], chunks: list[tuple[int, int, list[int]]],
            chunk_size: int = 1) -> dict[str, Any]:
    """A .tilemap of unit cells: palette entries (sheet handle, frame, solid) and chunks (x, y, tiles)."""
    return {
        "Format": "Tilemap", "Version": 1, "CellSize": [1, 1], "ChunkSize": chunk_size,
        "Palette": [{"Sheet": sheet, "Frame": frame, "Solid": solid} for sheet, frame, solid in palette],
        "Chunks": [{"X": x, "Y": y, "Tiles": tiles} for x, y, tiles in chunks],
    }


def baseline_entities() -> list[dict[str, Any]]:
    return [
        entity("e000000000000001", "Camera", {
            "Transform": transform(0, 2, 10), "Camera": {"Primary": True}, "AudioListener": {"Primary": True}}),
        entity("e000000000000002", "Sun", {"Transform": transform(0, 10, 0), "DirectionalLight": {"Intensity": 3}}),
    ]


class Fixture:
    """One validation fixture: its project files and its Expected.json fields."""

    def __init__(self, name: str, codes: list[str]) -> None:
        self.name = name
        self.codes = sorted(codes)
        self.project = default_project("Validation")
        self.project["StartScene"] = MAIN_SCENE
        self.project["Export"]["BuildScenes"] = [MAIN_SCENE]
        self.entities = baseline_entities()
        self.files: dict[str, bytes] = {}
        self.test_components = False
        self.expand: list[dict[str, Any]] = []
        self.non_canonical: list[str] = []
        self.scripts = False

    def add(self, path: str, data: str | bytes) -> None:
        self.files[path] = data.encode("utf-8") if isinstance(data, str) else data

    def add_json(self, path: str, document: dict[str, Any]) -> None:
        self.add(path, canonical(document))

    def add_script(self, path: str, handle: str, source: str) -> None:
        self.scripts = True
        self.add(path, source)
        self.add_json(path + ".meta", meta(handle, "Script", "Script"))

    def copy_asset(self, source: str, destination: str) -> None:
        self.add(destination, (ASSETS / source).read_bytes())

    def outputs(self) -> dict[str, bytes]:
        files = dict(self.files)
        files[f"Project/{PROJECT_FILE}"] = canonical(self.project).encode("utf-8")
        files[f"Project/{MAIN_SCENE}"] = canonical(scene("Main", self.entities)).encode("utf-8")
        files[f"Project/{MAIN_SCENE}.meta"] = canonical(meta(MAIN_SCENE_HANDLE, "Scene", "Scene")).encode("utf-8")
        if self.scripts:
            files["Project/.luaurc"] = b'{\n\t"languageMode": "strict"\n}\n'
        expected = {"Format": "ValidationFixture", "Version": 1, "Scope": "Project", "Codes": self.codes,
                    "TestComponents": self.test_components, "Expand": self.expand, "NonCanonical": self.non_canonical}
        files["Expected.json"] = canonical(expected).encode("utf-8")
        return {f"{self.name}/{path}": data for path, data in files.items()}


def body(identifier: str, name: str, x: float, body_type: str = "Static", collider: dict[str, Any] | None = None,
         parent: str | None = None, **rigid_body: Any) -> dict[str, Any]:
    components: dict[str, Any] = {"Transform": transform(x, 0, -20), "RigidBody": {"Type": body_type, **rigid_body}}
    components.update(collider or {"BoxCollider": {"HalfExtents": [0.5, 0.5, 0.5]}})
    return entity(identifier, name, components, parent)


def body_2d(identifier: str, name: str, x: float, body_type: str = "Static", collider: dict[str, Any] | None = None,
            parent: str | None = None, **rigid_body: Any) -> dict[str, Any]:
    """body() in the 2D world: a RigidBody2D with a 2D collider (a unit BoxCollider2D by default)."""
    components: dict[str, Any] = {"Transform": transform(x, 0, -20), "RigidBody2D": {"Type": body_type, **rigid_body}}
    components.update(collider or {"BoxCollider2D": {"HalfExtents": [0.5, 0.5]}})
    return entity(identifier, name, components, parent)


BEHAVIOUR = '''--!strict
export type Mover = ScriptInstance & {
\tSpeed: number,
}

local Mover = {}

Mover.Fields = {
\tSpeed = Field.Number(1, { Min = 0 }),
}

function Mover.OnFixedUpdate(self: Mover, dt: number)
\tself.Entity.Transform:Translate(vector.create(self.Speed * dt, 0, 0))
end

return Script.Define("Mover", Mover)
'''

MODULE = '''--!strict
local Helpers = {}

function Helpers.Add(left: number, right: number): number
\treturn left + right
end

return Helpers
'''

# A module whose Assets.Load takes a path it computes (Docs/Decisions/0030-m19-contract.md decision 21): with
# Export.PruneUnreferenced on, project.validate names the call (SCRIPT_COMPUTED_ASSET_LOAD).
COMPUTED_LOADER = '''--!strict
local Loader = {}

function Loader.Load(name: string)
\treturn Assets.Load("Assets/Textures/" .. name .. ".png")
end

return Loader
'''


def validation_fixtures() -> list[Fixture]:
    fixtures: list[Fixture] = []

    def fixture(name: str, codes: list[str] | None = None) -> Fixture:
        item = Fixture(name, [name] if codes is None else codes)
        fixtures.append(item)
        return item

    fixture("Clean", [])

    item = fixture("SCENE_NO_PRIMARY_CAMERA")
    item.entities[0]["Components"]["Camera"]["Primary"] = False

    item = fixture("SCENE_MULTIPLE_PRIMARY_CAMERAS")
    item.entities.append(entity("e000000000000003", "SecondCamera", {
        "Transform": transform(5, 2, 10), "Camera": {"Primary": True}}))

    item = fixture("SCENE_DUPLICATE_UNIQUE_COMPONENT")
    item.entities.append(entity("e000000000000003", "GradeA", {
        "Transform": transform(), "PostProcess": {"ExposureEV": 0.5}}))
    item.entities.append(entity("e000000000000004", "GradeB", {
        "Transform": transform(), "PostProcess": {"ExposureEV": -0.5}}))

    item = fixture("SCENE_NONCANONICAL_ORDER")
    child = entity("e000000000000004", "Child", {"Transform": transform(0, 1, 0)}, parent="e000000000000003")
    item.entities.append(child)
    item.entities.append(entity("e000000000000003", "Parent", {"Transform": transform()}))

    item = fixture("SCENE_INVALID_HIERARCHY")
    item.entities.append(entity("e000000000000003", "Orphan", {"Transform": transform()}, parent="e0000000000000ff"))

    item = fixture("SCENE_INCONSISTENT_PREFAB_LINK")
    item.entities.append(entity("e000000000000003", "Member", {
        "Transform": transform(),
        "PrefabLink": {"PrefabEntityID": "00000000000b0002", "InstanceRoot": "e0000000000000ff",
                       "OuterPrefabEntityID": None}}))

    item = fixture("ENTITY_DUPLICATE_ID")
    item.entities.append(entity("e000000000000003", "First", {"Transform": transform()}))
    item.entities.append(entity("e000000000000003", "Second", {"Transform": transform(1, 0, 0)}))

    item = fixture("ENTITY_DANGLING_REFERENCE")
    item.test_components = True
    item.entities.append(entity("e000000000000003", "Follower", {
        "Transform": transform(), "ValidatorTarget": {"Target": "e0000000000000ff"}}))

    item = fixture("COMPONENT_FIELD_OUT_OF_RANGE")
    item.entities.append(entity("e000000000000003", "Lamp", {"Transform": transform(), "PointLight": {"Range": -1}}))

    item = fixture("COMPONENT_MISSING_REQUIREMENT")
    item.test_components = True
    item.entities.append(entity("e000000000000003", "Follower", {
        "Transform": transform(), "ValidatorFollower": {"Speed": 2}}))

    item = fixture("COMPONENT_CONFLICT")
    item.entities.append(entity("e000000000000003", "Player", {
        "Transform": transform(0, 0, -20), "RigidBody": {"Type": "Kinematic"}, "CapsuleCollider": {},
        "CharacterController": {}}))

    item = fixture("ASSET_MISSING")
    item.entities.append(entity("e000000000000003", "Ghost", {
        "Transform": transform(), "MeshRenderer": {"Mesh": "00000000abcdef01"}}))

    item = fixture("ASSET_TYPE_MISMATCH", ["ASSET_IMPORT_FAILED", "ASSET_TYPE_MISMATCH"])
    item.copy_asset("Materials/Red.material", "Project/Assets/Materials/Red.material")
    item.add_json("Project/Assets/Materials/Red.material.meta", meta("3a7e000000000001", "Material", "Material"))
    item.entities.append(entity("e000000000000003", "Wrong", {
        "Transform": transform(), "MeshRenderer": {"Mesh": "3a7e000000000001"}}))
    # A .meta whose type disagrees with its source's extension: the scan reports it as autoFixable (the fix rewrites it
    # for the importer that takes the source, keeping its handle), so GetCodeInfos documents the code as fixable.
    # Loading the asset fails as a consequence (the importer produces a Material, not the Texture the .meta names):
    # ASSET_IMPORT_FAILED.
    item.copy_asset("Materials/Red.material", "Project/Assets/Materials/Typed.material")
    item.add_json("Project/Assets/Materials/Typed.material.meta", meta("3a7e000000000007", "Texture", "Material"))

    item = fixture("ASSET_IMPORT_FAILED")
    item.copy_asset("Textures/Truncated.png", "Project/Assets/Textures/Truncated.png")
    item.add_json("Project/Assets/Textures/Truncated.png.meta",
                  meta("3a7e000000000002", "Texture", "Texture", TEXTURE_SETTINGS))
    item.copy_asset("Textures/Grey.png", "Project/Assets/Textures/Unreadable.png")
    item.add("Project/Assets/Textures/Unreadable.png.meta", '{"Format": "AssetMeta", "Version": 1, "Handle":\n')
    item.non_canonical.append("Assets/Textures/Unreadable.png.meta")

    item = fixture("ASSET_ORPHAN_META")
    item.add_json("Project/Assets/Textures/Gone.png.meta",
                  meta("3a7e000000000003", "Texture", "Texture", TEXTURE_SETTINGS))

    item = fixture("ASSET_DUPLICATE_HANDLE")
    for name in ("Grey.png", "Grey Copy.png"):
        item.copy_asset("Textures/Grey.png", f"Project/Assets/Textures/{name}")
        item.add_json(f"Project/Assets/Textures/{name}.meta",
                      meta("3a7e000000000004", "Texture", "Texture", TEXTURE_SETTINGS))

    item = fixture("ASSET_ORPHAN_DEPENDENCY")
    item.add("Project/Assets/Models/Orphan.bin", bytes(range(16)))
    item.add_json("Project/Assets/Models/Orphan.bin.meta",
                  {"Format": "AssetMeta", "Version": 1, "Handle": "3a7e000000000005", "Type": "Dependency",
                   "Owner": "3a7e0000000000ff"})

    # ASSET_VERTEX_COLORS_IGNORED has no fixture: GltfImporter version 2 imports COLOR_0, so nothing reports it any more
    # (ProjectValidator::GetRetiredCodes; Docs/Decisions/0034-m23-contract.md decision 18).
    for code, source in (("ASSET_TANGENTS_APPROXIMATED", "NoNormalsNoUvs.gltf"),
                         ("ASSET_UNSUPPORTED_UV_SET", "TexCoord2Occlusion.gltf"),
                         ("ASSET_CONTENT_SKIPPED", "SkippedContent.gltf")):
        item = fixture(code)
        item.copy_asset(f"Gltf/{source}", f"Project/Assets/Models/{source}")

    item = fixture("PATH_CASE_MISMATCH")
    item.copy_asset("Textures/Grey.png", "Project/Assets/Textures/Grey.png")
    item.add_json("Project/Assets/Textures/grey.png.meta",
                  meta("3a7e000000000006", "Texture", "Texture", TEXTURE_SETTINGS))

    item = fixture("PREFAB_MISSING_ASSET")
    item.entities.append(entity("e000000000000003", "Instance", {
        "Transform": transform(), "Prefab": {"Prefab": "00000000abcdef02", "Overrides": []}}))

    # A prefab whose document holds a live instance of itself (Docs/Decisions/0031-m20-contract.md decision 8). The file
    # loads and imports on its own; only the nesting graph shows the cycle.
    item = fixture("PREFAB_CYCLE")
    item.add_json("Project/Assets/Prefabs/Loop.prefab", prefab("Loop", "00000000000a0001", [
        entity("00000000000a0001", "Loop", {"Transform": transform()}),
        entity("00000000000a0002", "Inner", {
            "Transform": transform(1, 0, 0), "Prefab": {"Prefab": "3a7e000000000008", "Overrides": []},
            "PrefabLink": {"PrefabEntityID": "00000000000a0001", "InstanceRoot": "00000000000a0002",
                           "OuterPrefabEntityID": None}}, parent="00000000000a0001"),
    ]))
    item.add_json("Project/Assets/Prefabs/Loop.prefab.meta", meta("3a7e000000000008", "Prefab", "Prefab"))

    item = fixture("PHYSICS_NONCONVEX_DYNAMIC")
    item.entities.append(body("e000000000000003", "Rock", 0, "Dynamic",
                              {"MeshCollider": {"Mesh": CUBE_MESH, "Convex": False}}))

    item = fixture("PHYSICS_MIXED_TRIGGER")
    item.entities.append(body("e000000000000003", "Mixed", 0, "Static",
                              {"BoxCollider": {"IsTrigger": True}, "SphereCollider": {"IsTrigger": False}}))

    item = fixture("PHYSICS_DYNAMIC_TRIGGER")
    item.entities.append(body("e000000000000003", "Sensor", 0, "Dynamic", {"BoxCollider": {"IsTrigger": True}}))

    item = fixture("PHYSICS_ALL_DOFS_LOCKED")
    item.entities.append(body("e000000000000003", "Frozen", 0, "Dynamic", None, LockTranslation=[True, True, True],
                              LockRotation=[True, True, True]))

    item = fixture("PHYSICS_INVALID_SHAPE")
    item.entities.append(body("e000000000000003", "Sheet", 0, "Static",
                              {"MeshCollider": {"Mesh": PLANE_MESH, "Convex": True}}))

    # Collider-only entities are implicit static bodies; two that touch bump a rolling body at their seam.
    item = fixture("PHYSICS_ADJACENT_STATIC_BODIES")
    item.entities.append(entity("e000000000000003", "TrackA", {
        "Transform": transform(0, 0, -20), "BoxCollider": {"HalfExtents": [0.5, 0.5, 0.5]}}))
    item.entities.append(entity("e000000000000004", "TrackB", {
        "Transform": transform(1, 0, -20), "BoxCollider": {"HalfExtents": [0.5, 0.5, 0.5]}}))

    item = fixture("PHYSICS_UNKNOWN_LAYER")
    item.entities.append(body("e000000000000003", "Lost", 0, "Static", None, Layer="Nowhere"))

    item = fixture("PHYSICS_NONUNIFORM_SCALE")
    stretched = body("e000000000000003", "Egg", 0, "Static", {"SphereCollider": {"Radius": 0.5}})
    stretched["Components"]["Transform"]["Scale"] = [1, 2, 1]
    item.entities.append(stretched)

    item = fixture("PHYSICS_DYNAMIC_UNDER_MOVING_PARENT")
    item.entities.append(body("e000000000000003", "Platform", 0, "Kinematic"))
    item.entities.append(body("e000000000000004", "Crate", 0, "Dynamic", parent="e000000000000003"))
    item.entities[-1]["Components"]["Transform"]["Translation"] = [0, 2, 0]

    item = fixture("PHYSICS_LIMIT_EXCEEDED")
    item.entities.append(body("e000000000000003", "Pebble", 0, "Dynamic", {"SphereCollider": {"Radius": 0.25}}))
    item.expand.append({"Path": MAIN_SCENE, "Entity": "e000000000000003", "Count": 16385})

    item = fixture("SCRIPT_COMPILE_ERROR")
    item.add_script("Project/Assets/Scripts/Broken.luau", "5c41000000000001",
                    "--!strict\n-- A module whose return statement does not parse.\nreturn )\n")

    item = fixture("SCRIPT_TYPE_ERROR")
    item.add_script("Project/Assets/Scripts/Typed.luau", "5c41000000000002",
                    "--!strict\nlocal count: number = \"three\"\n\nreturn { Count = count }\n")

    item = fixture("SCRIPT_NOT_A_BEHAVIOUR")
    item.add_script("Project/Assets/Scripts/Helpers.luau", "5c41000000000003", MODULE)
    item.entities.append(entity("e000000000000003", "Helper", {
        "Transform": transform(),
        "Script": {"Scripts": [{"Script": "5c41000000000003", "Fields": {}, "ExecutionOrder": 0}]}}))

    item = fixture("SCRIPT_UNKNOWN_FIELD_OVERRIDE")
    item.add_script("Project/Assets/Scripts/Mover.luau", "5c41000000000004", BEHAVIOUR)
    item.entities.append(entity("e000000000000003", "Mover", {
        "Transform": transform(),
        "Script": {"Scripts": [{"Script": "5c41000000000004", "Fields": {"Sped": 2}, "ExecutionOrder": 0}]}}))

    item = fixture("SCRIPT_FIELD_TYPE_MISMATCH")
    item.add_script("Project/Assets/Scripts/Mover.luau", "5c41000000000004", BEHAVIOUR)
    item.entities.append(entity("e000000000000003", "Mover", {
        "Transform": transform(),
        "Script": {"Scripts": [{"Script": "5c41000000000004", "Fields": {"Speed": "fast"}, "ExecutionOrder": 0}]}}))

    item = fixture("INPUT_UNKNOWN_ACTION")
    item.add_script("Project/Assets/Scripts/Jumper.luau", "5c41000000000005", '''--!strict
local Jumper = {}

function Jumper.OnFixedUpdate(self: ScriptInstance, dt: number)
\tif Input.IsActionPressed("Jump") then
\t\tself.Entity.Transform:Translate(vector.create(0, dt, 0))
\tend
end

return Script.Define("Jumper", Jumper)
''')

    item = fixture("AUDIO_NO_LISTENER", ["AUDIO_NO_LISTENER", "SCENE_NO_PRIMARY_CAMERA"])
    item.entities = [
        entity("e000000000000002", "Sun", {"Transform": transform(0, 10, 0), "DirectionalLight": {"Intensity": 3}}),
        entity("e000000000000003", "Speaker", {"Transform": transform(), "AudioSource": {"Spatial": True}}),
    ]

    item = fixture("AUDIO_MULTIPLE_PRIMARY_LISTENERS")
    item.entities.append(entity("e000000000000003", "Ears", {
        "Transform": transform(), "AudioListener": {"Primary": True}}))

    item = fixture("RENDER_NO_LIGHTING")
    item.entities = [
        entity("e000000000000001", "Camera", {
            "Transform": transform(0, 2, 10), "Camera": {"Primary": True}, "AudioListener": {"Primary": True}}),
        entity("e000000000000002", "Environment", {
            "Transform": transform(), "Environment": {"FallbackColor": [0, 0, 0]}}),
        entity("e000000000000003", "Cube", {"Transform": transform(), "MeshRenderer": {"Mesh": CUBE_MESH}}),
    ]

    item = fixture("RENDER_LIGHT_LIMIT_EXCEEDED")
    item.entities.append(entity("e000000000000003", "Bulb", {
        "Transform": transform(0, 3, 0), "PointLight": {"Range": 2}}))
    item.expand.append({"Path": MAIN_SCENE, "Entity": "e000000000000003", "Count": 256})

    item = fixture("RENDER_SPOT_SHADOW_BUDGET")
    for index in range(9):
        item.entities.append(entity(f"e00000000000001{index}", f"Spot{index}", {
            "Transform": transform(index * 2, 4, 0), "SpotLight": {"CastShadows": True}}))

    item = fixture("BUILD_START_SCENE_MISSING")
    item.project["StartScene"] = "Assets/Scenes/Missing.scene"

    item = fixture("BUILD_SCENE_MISSING")
    item.project["Export"]["BuildScenes"] = [MAIN_SCENE, "Assets/Scenes/Missing.scene"]

    item = fixture("TEST_SUITE_INVALID")
    item.project["Testing"]["Suites"] = [{
        "Script": "Assets/Tests/Missing.test.luau", "Scene": "", "Parameters": None, "Isolation": "Suite", "Clock": [],
        "Overrides": {"CallbackBudgetMs": 0, "MemoryLimitMB": 0, "PauseOnError": "Inherit"}, "ExpectQuit": -1,
        "Modes": ["Editor", "Release", "Dist"]}]

    item = fixture("SCRIPT_COMPUTED_ASSET_LOAD")
    item.project["Export"]["PruneUnreferenced"] = True
    item.add_script("Project/Assets/Scripts/Loader.luau", "5c41000000000006", COMPUTED_LOADER)

    # M21 (Docs/Decisions/0032-m21-contract.md decision 12): a sprite in a sorting layer SortingLayers.Layers does not
    # declare, beside a tilemap renderer in another (decision 30), and one whose Frame the sheet lacks.
    item = fixture("SPRITE_UNKNOWN_SORTING_LAYER")
    item.entities.append(entity("e000000000000003", "Sprite", {
        "Transform": transform(), "SpriteRenderer": {"SortingLayer": "Sky"}}))
    item.entities.append(entity("e000000000000004", "Tiles", {
        "Transform": transform(), "TilemapRenderer": {"SortingLayer": "Ground"}}))

    item = fixture("SPRITE_FRAME_OUT_OF_RANGE")
    item.add_json("Project/Assets/Sprites/Hero.spritesheet", sprite_sheet([("Idle", 0, 0)], [], 16))
    item.add_json("Project/Assets/Sprites/Hero.spritesheet.meta",
                  meta("3a7e000000000009", "SpriteSheet", "SpriteSheet"))
    item.entities.append(entity("e000000000000003", "Sprite", {
        "Transform": transform(), "SpriteRenderer": {"Sheet": "3a7e000000000009", "Frame": 1}}))

    # M21 (Docs/Decisions/0032-m21-contract.md decision 12): a SpriteAnimator naming a clip its sheet lacks.
    item = fixture("SPRITE_UNKNOWN_CLIP")
    item.add_json("Project/Assets/Sprites/Hero.spritesheet", sprite_sheet([("Idle", 0, 0)], [("Idle", [(0, 4)])]))
    item.add_json("Project/Assets/Sprites/Hero.spritesheet.meta",
                  meta("3a7e000000000009", "SpriteSheet", "SpriteSheet"))
    item.entities.append(entity("e000000000000003", "Hero", {
        "Transform": transform(), "SpriteRenderer": {"Sheet": "3a7e000000000009"}, "SpriteAnimator": {"Clip": "Run"}}))

    # M21 (Docs/Decisions/0032-m21-contract.md decision 30): a tilemap whose second palette entry names a frame its
    # one-frame sheet lacks.
    item = fixture("TILEMAP_FRAME_OUT_OF_RANGE")
    item.add_json("Project/Assets/Sprites/Tiles.spritesheet", sprite_sheet([("Ground", 0, 0)], []))
    item.add_json("Project/Assets/Sprites/Tiles.spritesheet.meta",
                  meta("3a7e00000000000a", "SpriteSheet", "SpriteSheet"))
    item.add_json("Project/Assets/Tilemaps/Level.tilemap",
                  tilemap([("3a7e00000000000a", 0, True), ("3a7e00000000000a", 3, False)], [(0, 0, [1]), (1, 0, [2])]))
    item.add_json("Project/Assets/Tilemaps/Level.tilemap.meta", meta("7113000000000001", "Tilemap", "Tilemap"))

    # M21's milestone review (Docs/Decisions/0032-m21-contract.md): a sheet of the 4 x 4 Grey.png whose second
    # frame, 4 x 4 texels at (2, 2), extends past it, reported on the sheet, and a tilemap whose second palette entry
    # names that frame, reported on the tilemap; the first frame and entry lie inside the texture.
    item = fixture("SPRITE_FRAME_OUTSIDE_TEXTURE")
    item.copy_asset("Textures/Grey.png", "Project/Assets/Textures/Grey.png")
    item.add_json("Project/Assets/Textures/Grey.png.meta",
                  meta("3a7e00000000000b", "Texture", "Texture", TEXTURE_SETTINGS))
    item.add_json("Project/Assets/Sprites/Tiles.spritesheet",
                  sprite_sheet([("Inside", 0, 0), ("Past", 2, 2)], [], 4, "3a7e00000000000b"))
    item.add_json("Project/Assets/Sprites/Tiles.spritesheet.meta",
                  meta("3a7e00000000000c", "SpriteSheet", "SpriteSheet"))
    item.add_json("Project/Assets/Tilemaps/Level.tilemap",
                  tilemap([("3a7e00000000000c", 0, True), ("3a7e00000000000c", 1, False)], [(0, 0, [1]), (1, 0, [2])]))
    item.add_json("Project/Assets/Tilemaps/Level.tilemap.meta", meta("7113000000000001", "Tilemap", "Tilemap"))

    # M21 (Docs/Decisions/0032-m21-contract.md decision 23; W21.2's review): a tilemap's palette sheets are checked as
    # a sprite sheet's texture is, each in a second fixture of its code named <CODE>.Tilemap: a palette entry naming a
    # sheet that is not registered, and one naming a registered asset of another type. The null entry before it is no
    # issue.
    item = fixture("ASSET_MISSING.Tilemap", ["ASSET_MISSING"])
    item.add_json("Project/Assets/Tilemaps/Level.tilemap",
                  tilemap([(None, 0, True), ("00000000abcdef02", 0, True)], [(0, 0, [1]), (1, 0, [2])]))
    item.add_json("Project/Assets/Tilemaps/Level.tilemap.meta", meta("7113000000000001", "Tilemap", "Tilemap"))

    item = fixture("ASSET_TYPE_MISMATCH.Tilemap", ["ASSET_TYPE_MISMATCH"])
    item.copy_asset("Materials/Red.material", "Project/Assets/Materials/Red.material")
    item.add_json("Project/Assets/Materials/Red.material.meta", meta("3a7e000000000001", "Material", "Material"))
    item.add_json("Project/Assets/Tilemaps/Level.tilemap",
                  tilemap([(None, 0, True), ("3a7e000000000001", 0, True)], [(0, 0, [1]), (1, 0, [2])]))
    item.add_json("Project/Assets/Tilemaps/Level.tilemap.meta", meta("7113000000000001", "Tilemap", "Tilemap"))

    # M21 (Docs/Decisions/0032-m21-contract.md decision 12). An entity with 2D and 3D physics components stays in the 3D
    # world; its 2D components make no body.
    item = fixture("PHYSICS2D_MIXED_WORLDS")
    item.entities.append(entity("e000000000000003", "Mixed", {
        "Transform": transform(0, 0, -20), "BoxCollider": {"HalfExtents": [0.5, 0.5, 0.5]},
        "RigidBody2D": {"Type": "Static"}, "CircleCollider2D": {"Radius": 0.5}}))

    # A 2D body rotated 30 degrees about X: it takes its rotation about Z (none) and is reported.
    item = fixture("PHYSICS2D_OFF_PLANE")
    tilted = entity("e000000000000003", "Tilted", {
        "Transform": transform(0, 0, -20), "RigidBody2D": {"Type": "Static"},
        "BoxCollider2D": {"HalfExtents": [0.5, 0.5]}})
    tilted["Components"]["Transform"]["Rotation"] = [0.25881904, 0, 0, 0.9659258]
    item.entities.append(tilted)

    # M21 (Docs/Decisions/0032-m21-contract.md decision 12; W21.1's review): the rules the 2D world shares with the 3D
    # one, reported under the 3D codes for the 2D components, each in a second fixture of its code named <CODE>.2D.
    item = fixture("PHYSICS_UNKNOWN_LAYER.2D", ["PHYSICS_UNKNOWN_LAYER"])
    item.entities.append(body_2d("e000000000000003", "Lost", 0, "Static", None, Layer="Nowhere"))

    item = fixture("PHYSICS_MIXED_TRIGGER.2D", ["PHYSICS_MIXED_TRIGGER"])
    item.entities.append(body_2d("e000000000000003", "Mixed", 0, "Static",
                                 {"BoxCollider2D": {"IsTrigger": True}, "CircleCollider2D": {"IsTrigger": False}}))

    item = fixture("PHYSICS_DYNAMIC_TRIGGER.2D", ["PHYSICS_DYNAMIC_TRIGGER"])
    item.entities.append(body_2d("e000000000000003", "Sensor", 0, "Dynamic", {"BoxCollider2D": {"IsTrigger": True}}))

    # Points on one line have no convex hull.
    item = fixture("PHYSICS_INVALID_SHAPE.2D", ["PHYSICS_INVALID_SHAPE"])
    item.entities.append(body_2d("e000000000000003", "Sliver", 0, "Static",
                                 {"PolygonCollider2D": {"Points": [[-1, 0], [0, 0], [1, 0]]}}))

    item = fixture("PHYSICS_NONUNIFORM_SCALE.2D", ["PHYSICS_NONUNIFORM_SCALE"])
    stretched = body_2d("e000000000000003", "Egg", 0, "Static", {"CircleCollider2D": {"Radius": 0.5}})
    stretched["Components"]["Transform"]["Scale"] = [2, 1, 1]
    item.entities.append(stretched)

    item = fixture("PHYSICS_DYNAMIC_UNDER_MOVING_PARENT.2D", ["PHYSICS_DYNAMIC_UNDER_MOVING_PARENT"])
    item.entities.append(body_2d("e000000000000003", "Platform", 0, "Kinematic"))
    item.entities.append(body_2d("e000000000000004", "Crate", 0, "Dynamic", parent="e000000000000003"))
    item.entities[-1]["Components"]["Transform"]["Translation"] = [0, 2, 0]

    item = fixture("PHYSICS_LIMIT_EXCEEDED.2D", ["PHYSICS_LIMIT_EXCEEDED"])
    item.entities.append(body_2d("e000000000000003", "Pebble", 0, "Dynamic", {"CircleCollider2D": {"Radius": 0.25}}))
    item.expand.append({"Path": MAIN_SCENE, "Entity": "e000000000000003", "Count": 16385})

    # Five shadow-casting point lights, one more than a view shadows (Docs/Decisions/0034-m23-contract.md decision 18).
    item = fixture("RENDER_POINT_SHADOW_BUDGET")
    for index in range(5):
        item.entities.append(entity(f"e00000000000002{index}", f"Bulb{index}", {
            "Transform": transform(index * 2, 3, 0), "PointLight": {"CastShadows": True}}))

    # The animation checks (Docs/Decisions/0034-m23-contract.md decision 30): a mesh skinned to a three-joint skin,
    # drawn by a SkinnedMesh with no Animator above it, and drawn below an Animator posing the one-joint skeleton of
    # another file.
    rig_handle, bone_handle = 0x3a7e000000000011, 0x3a7e000000000012
    body_mesh = hash64(rig_handle, "mesh:0:Body")
    skinned_body = {"Transform": transform(), "MeshRenderer": {"Mesh": body_mesh}, "SkinnedMesh": {"MorphWeights": []}}

    def add_rigs(target: Fixture) -> None:
        for name, handle, mesh, joints in (("Rig", rig_handle, "Body", ["Root", "Mid", "Tip"]),
                                           ("Bone", bone_handle, "Tail", ["Bone"])):
            target.add(f"Project/Assets/Models/{name}.gltf", skinned_gltf(mesh, name, joints))
            target.add_json(f"Project/Assets/Models/{name}.gltf.meta",
                            meta(f"{handle:016x}", "Prefab", "Gltf", GLTF_SETTINGS))

    item = fixture("ANIMATION_NO_ANIMATOR")
    add_rigs(item)
    item.entities.append(entity("e000000000000030", "Body", skinned_body))

    item = fixture("ANIMATION_SKELETON_MISMATCH")
    add_rigs(item)
    item.entities.append(entity("e000000000000031", "Hero", {
        "Transform": transform(),
        "Animator": {"Skeleton": hash64(bone_handle, "skin:0:Bone"), "Layers": [], "Playing": True,
                     "RootMotion": "None"}}))
    item.entities.append(entity("e000000000000032", "Body", skinned_body, parent="e000000000000031"))

    return fixtures


README = """# Validation fixtures

Generated by `Tests/Data/Generate/MakeValidationFixtures.py`; do not edit by hand (ADR 0022 decision 11). Each
directory is named after the `project.validate` code it triggers, or `<CODE>.<Variant>` for a further fixture of that
code (`.2D`: the 2D world's composition, ADR 0032 decision 12; `.Tilemap`: a tilemap's palette sheets, its decision 23),
or `Clean`, the baseline that reports nothing.
`Project/` is a complete mini project; `Expected.json` lists every code it reports (`Codes`, sorted), whether the
validator test components are registered (`TestComponents`), the entities the test repeats into a scene (`Expand`)
and the deliberately malformed files (`NonCanonical`). The test "Validation: each fixture reports exactly its expected
codes" copies each project into a temporary directory, opens it, loads its assets (scenes and scripts excepted: the
validator and the script service check those) and runs `ProjectValidator::Validate` with the project scope; coverage
gate 6 requires a fixture for every code but the retired ones (`ProjectValidator::GetRetiredCodes`), which nothing
reports: `ASSET_VERTEX_COLORS_IGNORED` since GltfImporter version 2 imports `COLOR_0` (M23).

- `COMPONENT_MISSING_REQUIREMENT` and `ENTITY_DANGLING_REFERENCE` use the test components `ValidatorTarget` and
  `ValidatorFollower`: every built-in requirement is Transform, which every entity has.
- `PATH_CASE_MISMATCH` pairs `Grey.png` with `grey.png.meta`; the names differ in more than letter case, so git checks
  both out everywhere and the scan reports the mismatch on case-sensitive and case-insensitive file systems alike.
- `PHYSICS_LIMIT_EXCEEDED` and `PHYSICS_LIMIT_EXCEEDED.2D` (16,385 bodies) and `RENDER_LIGHT_LIMIT_EXCEEDED` (257
  lights) are expanded at test time.
- `ASSET_MISSING.Tilemap` and `ASSET_TYPE_MISMATCH.Tilemap` name a sprite sheet from a tilemap's palette. Sheets have no
  placeholder, so the asset manager logs nothing for them and only the validator reports them.
- `ANIMATION_NO_ANIMATOR` and `ANIMATION_SKELETON_MISMATCH` reference the meshes and skeletons of generated skinned glTF
  files by their sub-asset handles, so their `.meta` files fix the source handles.
"""

# ---------------------------------------------------------------------------------------------------------------------
# The settings observation project (Tests/Data/Project/AllSettings.eproj names these files)
# ---------------------------------------------------------------------------------------------------------------------

OBSERVATION_SUITE = '''--!strict
-- Observes the suite settings of Tests/Data/Project/AllSettings.eproj through the runner (gate 8). Every case passes \
only
-- when its setting reached the session: the Scene, the Parameters, the scripted Clock (fewer frames than ticks), case
-- Isolation, CaseTimeoutTicks and SuiteTickLimit (the long cases outlast both defaults) and ExpectQuit (the last case).
local LongCases = 31
local LongTicks = 1180

return Test.Suite("Main", function()
\tTest.Case("the suite runs in its scene", function()
\t\tTest.Expect(Scene.FindByName("Ball") ~= nil, "Main.scene has a Ball")
\tend)

\tTest.Case("parameters reach the scene", function()
\t\tlocal parameters = Scene.GetLoadParameters()
\t\tTest.ExpectEqual(parameters.Level, 2)
\t\tTest.ExpectNear(parameters.Time :: number, 61.25)
\tend)

\tTest.Case("a scripted clock does not step once per frame", function()
\t\tlocal frame, tick = Time.GetFrameCount(), Time.GetTick()
\t\tTest.WaitTicks(10)
\t\tTest.Expect(Time.GetFrameCount() - frame < Time.GetTick() - tick, "fewer frames than ticks")
\tend)

\tTest.Case("case isolation restarts the scene", function()
\t\tTest.Expect(Scene.FindByName("Marker") == nil, "no marker survives an earlier case")
\t\tScene.CreateEntity("Marker")
\tend)

\tTest.Case("case isolation restarts the scene again", function()
\t\tTest.Expect(Scene.FindByName("Marker") == nil, "no marker survives an earlier case")
\t\tScene.CreateEntity("Marker")
\tend)

\tfor index = 1, LongCases do
\t\tTest.Case("a long case " .. tostring(index), function()
\t\t\tTest.WaitTicks(LongTicks)
\t\tend)
\tend

\tTest.Case("the suite quits with its expected code", function()
\t\tApplication.Quit(3)
\tend)
end)
'''

OBSERVATION_BEHAVIOUR = '''--!strict
local Probe = {}

function Probe.OnFixedUpdate(self: ScriptInstance, dt: number)
\tif Input.IsActionDown("Jump") then
\t\tself.Entity.Transform:Translate(vector.create(0, dt, 0))
\tend
\tlocal axis = Input.GetAxis("MoveX")
\tif axis ~= 0 then
\t\tself.Entity.Transform:Translate(vector.create(axis * dt, 0, 0))
\tend
end

return Script.Define("Probe", Probe)
'''


def observation_files() -> dict[str, bytes]:
    files: dict[str, bytes] = {}

    def add_json(path: str, document: dict[str, Any]) -> None:
        files[path] = canonical(document).encode("utf-8")

    main = scene("Main", [
        entity("0b5e000000000001", "Camera", {
            "Transform": transform(0, 2, 10), "Camera": {"Primary": True}, "AudioListener": {"Primary": True}}),
        entity("0b5e000000000002", "Sun", {"Transform": transform(0, 10, 0), "DirectionalLight": {"Intensity": 3}}),
        entity("0b5e000000000003", "Ball", {
            "Transform": transform(0, 5, 0), "RigidBody": {"Type": "Dynamic", "Layer": "Ball"},
            "SphereCollider": {"Radius": 0.5}}),
        entity("0b5e000000000004", "Probe", {
            "Transform": transform(),
            "Script": {"Scripts": [{"Script": "0b5e0000000000a3", "Fields": {}, "ExecutionOrder": 0}]}}),
        # SortingLayers.Layers: Front comes first and has the lower OrderInLayer, so only the declared layers draw Back
        # before it in the session's extraction.
        entity("0b5e000000000005", "Front", {
            "Transform": transform(), "SpriteRenderer": {"SortingLayer": "Foreground", "OrderInLayer": 0}}),
        entity("0b5e000000000006", "Back", {
            "Transform": transform(), "SpriteRenderer": {"SortingLayer": "Background", "OrderInLayer": 5}}),
    ], seed=7)
    add_json("Assets/Scenes/Main.scene", main)
    add_json("Assets/Scenes/Main.scene.meta", meta("0b5e0000000000a1", "Scene", "Scene"))
    level = scene("Level2", [
        entity("0b5e000000000011", "Camera", {
            "Transform": transform(0, 2, 10), "Camera": {"Primary": True}, "AudioListener": {"Primary": True}}),
        entity("0b5e000000000012", "Sun", {"Transform": transform(0, 10, 0), "DirectionalLight": {"Intensity": 3}}),
    ])
    add_json("Assets/Scenes/Level2.scene", level)
    add_json("Assets/Scenes/Level2.scene.meta", meta("0b5e0000000000a2", "Scene", "Scene"))
    files["Assets/Scripts/Probe.luau"] = OBSERVATION_BEHAVIOUR.encode("utf-8")
    add_json("Assets/Scripts/Probe.luau.meta", meta("0b5e0000000000a3", "Script", "Script"))
    # The computed Assets.Load project.validate names while Export.PruneUnreferenced is on.
    files["Assets/Scripts/Loader.luau"] = COMPUTED_LOADER.encode("utf-8")
    add_json("Assets/Scripts/Loader.luau.meta", meta("0b5e0000000000a8", "Script", "Script"))
    files["Assets/Tests/Main.test.luau"] = OBSERVATION_SUITE.encode("utf-8")
    add_json("Assets/Tests/Main.test.luau.meta", meta("0b5e0000000000a4", "Script", "Script"))
    # The replay Testing.Replays' glob finds (the observation test lists it; a filtered run never verifies it).
    add_json("Assets/Tests/Replays/Main.replay", {
        "Format": "Replay", "Version": 3, "Scene": {"Handle": "0b5e0000000000a1", "Path": "Assets/Scenes/Main.scene"},
        "Parameters": {}, "Seed": 0, "FixedHz": 120, "EngineVersion": "0.1.0", "Config": "Editor", "Fingerprint": None,
        "Events": [], "Expect": [], "Checkpoints": [], "FinalTick": 1, "FinalStateHash": "0000000000000001"})
    add_json("Assets/Tests/Replays/Main.replay.meta", meta("0b5e0000000000a7", "Replay", "Replay"))
    files["Assets/Icon.png"] = checker_png(256)
    add_json("Assets/Icon.png.meta", meta("0b5e0000000000a5", "Texture", "Texture", TEXTURE_SETTINGS))
    # A file Export.Exclude's "Assets/Drafts/**" leaves out of an export.
    add_json("Assets/Drafts/Sketch.scene", scene("Sketch", []))
    add_json("Assets/Drafts/Sketch.scene.meta", meta("0b5e0000000000a6", "Scene", "Scene"))
    files[".luaurc"] = b'{\n\t"languageMode": "strict"\n}\n'
    return files


# ---------------------------------------------------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------------------------------------------------


def outputs() -> dict[Path, dict[str, bytes]]:
    validation: dict[str, bytes] = {"README.md": README.encode("utf-8")}
    names = set()
    for item in validation_fixtures():
        if item.name in names:
            raise ValueError(f"fixture {item.name} is defined twice")
        names.add(item.name)
        # <CODE>.<Variant> is a further fixture of <CODE>.
        if item.name != "Clean" and item.name.split(".", 1)[0] not in item.codes:
            raise ValueError(f"fixture {item.name} must report its own code")
        validation.update(item.outputs())
    return {VALIDATION: validation, OBSERVATION: observation_files()}


def owned_files(root: Path) -> set[str]:
    if not root.is_dir():
        return set()
    return {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog="Exit codes: 0 fixtures generated or verified, 1 stale or extra files or "
                                            "an I/O failure, 2 usage error.")
    parser.add_argument("--check", action="store_true", help="compare with the committed files; write nothing")
    parser.add_argument("--json", action="store_true", help="print a machine-readable report")
    arguments = parser.parse_args()
    changed: list[str] = []
    extra: list[str] = []
    try:
        for root, files in outputs().items():
            label = root.relative_to(ROOT).as_posix()
            for relative, data in sorted(files.items()):
                path = root / relative
                if not path.is_file() or path.read_bytes() != data:
                    changed.append(f"{label}/{relative}")
                    if not arguments.check:
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(data)
            for relative in sorted(owned_files(root) - files.keys()):
                extra.append(f"{label}/{relative}")
                if not arguments.check:
                    (root / relative).unlink()
        if not arguments.check:
            for root in (VALIDATION, OBSERVATION):
                for directory in sorted((entry for entry in root.rglob("*") if entry.is_dir()),
                                        key=lambda entry: len(entry.parts), reverse=True):
                    if not any(directory.iterdir()):
                        directory.rmdir()
    except (OSError, ValueError) as error:
        if arguments.json:
            print(json.dumps({"ok": False, "error": str(error)}))
        else:
            print(f"Validation fixtures: {error}", file=sys.stderr)
        return 1
    success = not arguments.check or not (changed or extra)
    if arguments.json:
        print(json.dumps({"ok": success, "changed": changed, "extra": extra}))
    else:
        verb = "stale" if arguments.check else "written"
        removed = "extra" if arguments.check else "removed"
        print(f"Validation fixtures: {len(changed)} {verb} file(s), {len(extra)} {removed} file(s)")
        for name in changed + extra:
            print(f"  {name}")
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
