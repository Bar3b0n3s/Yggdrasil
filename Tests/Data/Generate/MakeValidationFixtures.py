#!/usr/bin/env python3
"""Generate the project-validation fixtures and the settings observation project (ADR 0022 decisions 11 and 12).

Two output roots, each owned whole by this generator (a file it does not write is reported as extra and, without
--check, removed):
- Tests/Data/Validation/<Name>/: `Project/` (a complete mini project: the .eproj, a .luaurc when it has scripts,
  Assets/) and `Expected.json` ({"Format": "ValidationFixture", "Version": 1, "Scope": "Project", "Codes",
  "TestComponents", "Expand", "NonCanonical"}). <Name> is a code of ProjectValidator::GetCodes, whose Codes contain it,
  or Clean, the baseline (a primary camera with the audio listener, and a sun) that reports nothing. Every fixture
  starts from that baseline, so incidental codes stay out; Codes lists every code the project reports, sorted.
- Tests/Data/SettingsObservation/Project/: the files Tests/Data/Project/AllSettings.eproj names (Assets/Icon.png, the
  start, build and suite scenes, the suite script and its replay), opened by "ProjectSettings: every AllSettings.eproj
  value is observed by its consumer" together with that .eproj.

Special cases (also in Tests/Data/Validation/README.md, which this generator writes):
- COMPONENT_MISSING_REQUIREMENT and ENTITY_DANGLING_REFERENCE use the test components ValidatorTarget (an EntityRef
  field) and ValidatorFollower (requires ValidatorTarget), registered when TestComponents is true: every built-in
  Requires is Transform, which every entity has, and no user-facing built-in component has an EntityRef field.
- PATH_CASE_MISMATCH pairs Grey.png with grey.png.meta. The two names differ in more than letter case, so git checks
  both out on every file system, and the scan reports the meta whose name matches its source only ignoring case,
  whether the file system is case-sensitive or not.
- PHYSICS_LIMIT_EXCEEDED and RENDER_LIGHT_LIMIT_EXCEEDED name an entity in Expand ({"Path", "Entity", "Count"}): the
  test repeats it until the scene holds Count of it, so no multi-megabyte scene is committed.
- NonCanonical lists project-relative paths that are deliberately not canonical JSON (ASSET_IMPORT_FAILED's unreadable
  .meta); the authored-files test exempts exactly those.

Scenes, metas and project files are written in the canonical JsonWriter form; images are PNGs of stored deflate blocks,
so no zlib build changes their bytes. The glTF diagnostics fixtures and Truncated.png are byte copies of
Tests/Data/Assets files. Run from the repository root: without arguments it writes the files, `--check` compares and
writes nothing, `--json` prints a machine-readable report.

Exit codes: 0 fixtures generated or verified, 1 stale or extra files or an I/O failure, 2 usage error.
"""

from __future__ import annotations

import argparse
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
    "Script", "ValidatorTarget", "ValidatorFollower",
]

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
        "Export": {"BuildScenes": [], "Exclude": ["Assets/Tests/**"], "Icon": "", "Version": "1.0.0", "Company": ""},
        "Testing": {"Suites": [], "Replays": [], "CaseTimeoutTicks": 600, "SuiteTickLimit": 36000,
                    "CoverageGates": False},
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
    versions = {key: 1 for key in COMPONENT_ORDER if key in present}
    return {"Format": "Scene", "Version": 1, "Name": name, "Seed": seed, "ComponentVersions": versions,
            "Entities": entities}


def meta(handle: str, kind: str, importer: str, settings: dict[str, Any] | None = None) -> dict[str, Any]:
    return {"Format": "AssetMeta", "Version": 1, "Handle": handle, "Type": kind, "Importer": importer,
            "ImporterVersion": 1, "Settings": settings, "SubAssets": []}


TEXTURE_SETTINGS = {"Usage": "Color", "GenerateMips": True}


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
        "PrefabLink": {"PrefabEntityID": "00000000000b0002", "InstanceRoot": "e0000000000000ff"}}))

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

    for code, source in (("ASSET_TANGENTS_APPROXIMATED", "NoNormalsNoUvs.gltf"),
                         ("ASSET_UNSUPPORTED_UV_SET", "TexCoord1Occlusion.gltf"),
                         ("ASSET_VERTEX_COLORS_IGNORED", "VertexColors.gltf"),
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
        "Transform": transform(), "Script": {"Script": "5c41000000000003", "Fields": {}, "ExecutionOrder": 0}}))

    item = fixture("SCRIPT_UNKNOWN_FIELD_OVERRIDE")
    item.add_script("Project/Assets/Scripts/Mover.luau", "5c41000000000004", BEHAVIOUR)
    item.entities.append(entity("e000000000000003", "Mover", {
        "Transform": transform(),
        "Script": {"Script": "5c41000000000004", "Fields": {"Sped": 2}, "ExecutionOrder": 0}}))

    item = fixture("SCRIPT_FIELD_TYPE_MISMATCH")
    item.add_script("Project/Assets/Scripts/Mover.luau", "5c41000000000004", BEHAVIOUR)
    item.entities.append(entity("e000000000000003", "Mover", {
        "Transform": transform(),
        "Script": {"Script": "5c41000000000004", "Fields": {"Speed": "fast"}, "ExecutionOrder": 0}}))

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

    return fixtures


README = """# Validation fixtures

Generated by `Tests/Data/Generate/MakeValidationFixtures.py`; do not edit by hand (ADR 0022 decision 11). Each \
directory is
named after the `project.validate` code it triggers, or `Clean`, the baseline that reports nothing. `Project/` is a \
complete
mini project; `Expected.json` lists every code it reports (`Codes`, sorted), whether the validator test components are
registered (`TestComponents`), the entities the test repeats into a scene (`Expand`) and the deliberately malformed \
files
(`NonCanonical`). The test "Validation: each fixture reports exactly its expected codes" copies each project into a
temporary directory, opens it, loads its assets (scenes and scripts excepted: the validator and the script service check
those) and runs `ProjectValidator::Validate` with the project scope; coverage gate 6
requires a fixture for every code.

- `COMPONENT_MISSING_REQUIREMENT` and `ENTITY_DANGLING_REFERENCE` use the test components `ValidatorTarget` and
  `ValidatorFollower`: every built-in requirement is Transform, which every entity has.
- `PATH_CASE_MISMATCH` pairs `Grey.png` with `grey.png.meta`; the names differ in more than letter case, so git checks \
both
  out everywhere and the scan reports the mismatch on case-sensitive and case-insensitive file systems alike.
- `PHYSICS_LIMIT_EXCEEDED` (16,385 bodies) and `RENDER_LIGHT_LIMIT_EXCEEDED` (257 lights) are expanded at test time.
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
            "Transform": transform(), "Script": {"Script": "0b5e0000000000a3", "Fields": {}, "ExecutionOrder": 0}}),
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
    files["Assets/Tests/Main.test.luau"] = OBSERVATION_SUITE.encode("utf-8")
    add_json("Assets/Tests/Main.test.luau.meta", meta("0b5e0000000000a4", "Script", "Script"))
    # The replay Testing.Replays' glob finds (the observation test lists it; a filtered run never verifies it).
    add_json("Assets/Tests/Replays/Main.replay", {
        "Format": "Replay", "Version": 1, "Scene": {"Handle": "0b5e0000000000a1", "Path": "Assets/Scenes/Main.scene"},
        "Parameters": {}, "Seed": 0, "FixedHz": 120, "EngineVersion": "0.1.0", "Config": "Editor", "Events": [],
        "Expect": [], "FinalTick": 1, "FinalStateHash": "0000000000000001"})
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
        if item.name != "Clean" and item.name not in item.codes:
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
