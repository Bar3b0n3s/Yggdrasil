#!/usr/bin/env python3
"""Generate authored M13 script/type-checker fixtures; --check never writes files.

All sources are original test data. Templates are expanded from the shipped files,
so their fixture copies cannot silently diverge from script.create's input.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "Tests" / "Data" / "TypeChecker"


def fixtures() -> dict[str, str]:
    """Return deterministic UTF-8/LF sources and their expected diagnostics."""
    sources: dict[str, str] = {}
    for kind in ("Behaviour", "Module", "Test"):
        template = ROOT / "Resources" / "Templates" / "Scripts" / f"{kind}.luau"
        sources[f"Templates/{kind}.luau"] = template.read_text(encoding="utf-8").replace(
            "{{Name}}", f"Fixture{kind}"
        )
    sources["Fields.luau"] = '''--!strict
local Fields = {}
Fields.Fields = {
\tNumber = Field.Number(),
\tInteger = Field.Integer(),
\tBool = Field.Bool(),
\tString = Field.String(),
\tVector = Field.Vector(),
\tColor = Field.Color(),
\tQuat = Field.Quat(),
\tTarget = Field.Entity(),
\tAsset = Field.Asset("AudioClip"),
\tEnum = Field.Enum({ "First", "Second" }),
\tArray = Field.Array(Field.Array(Field.Number(0, { Min = 0, Max = 1, Step = 0.25 }))),
}
return Script.Define("Fields", Fields)
'''
    sources["Cast.luau"] = '''--!strict
local Behaviour = require("./Templates/Behaviour")
local function speed(entity: Entity): number
\tlocal instance = entity:GetScript() :: Behaviour.FixtureBehaviour?
\treturn if instance then instance.Speed else 0
end
return speed
'''
    sources["Bounds.luau"] = '''--!strict
local function bounds(entity: Entity)
\tlocal a, b = entity:GetWorldBounds()
\tlocal c, d = Physics.GetBodyBounds(entity)
\tlocal e, f = Physics.GetColliderBounds(entity)
\tif a and b then local size: vector = b - a; print(size) end
\tif c and d then local size: vector = d - c; print(size) end
\tif e and f then local size: vector = f - e; print(size) end
end
return bounds
'''
    components = (
        "Transform", "MeshRenderer", "Camera", "DirectionalLight", "PointLight",
        "SpotLight", "Environment", "PostProcess", "Text", "RigidBody",
        "BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider",
        "CharacterController", "AudioSource", "AudioListener", "Script",
    )
    component_calls = "".join(
        f'\tdo\n\t\tlocal component: {name}? = entity:GetComponent("{name}")\n'
        '\t\tif component then print(component) end\n\tend\n'
        for name in components
    )
    sources["ComponentOverloads.luau"] = (
        '--!strict\nlocal function check(entity: Entity, random: RandomGenerator)\n'
        + component_calls
        + '\tlocal choice: number = random:Choice({1, 2, 3})\n'
        + '\tlocal shuffled: {string} = random:Shuffle({"a", "b"})\n'
        + '\treturn choice, shuffled\nend\nreturn check\n'
    )
    sources["Errors/ComponentOverload.luau"] = '''--!strict
return function(entity: Entity): Transform?
\treturn entity:GetComponent("RigidBody")
end
'''
    sources["Errors/GenericMethod.luau"] = '''--!strict
return function(random: RandomGenerator): string
\treturn random:Choice({1, 2, 3})
end
'''
    sources["Errors/Type.luau"] = 'local value: number = "bad"\nreturn value\n'
    sources["Errors/Optional.luau"] = '''--!strict
local function bad(entity: Entity)
\tlocal low, high = entity:GetWorldBounds()
\treturn high - low
end
return bad
'''
    sources["Errors/Sealed.luau"] = '''--!strict
local function make(): { Value: number }
\treturn { Value = 1 }
end
local value = make()
value.Added = true
return value
'''
    expectations = []
    for path in sorted(sources):
        expected: dict[str, object] = {"Path": path, "Clean": not path.startswith("Errors/")}
        if path == "Errors/Type.luau":
            expected["Ranges"] = [[1, 23, 1, 28]]
        expectations.append(expected)
    sources["Expected.json"] = json.dumps(expectations, indent="\t", ensure_ascii=False) + "\n"
    return sources


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Exit codes: 0 fixtures generated or verified, 1 stale fixtures or I/O failure, 2 usage error.",
    )
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--json", action="store_true")
    arguments = parser.parse_args()
    changed: list[str] = []
    try:
        for relative, source in fixtures().items():
            path = OUTPUT / relative
            data = source.encode("utf-8")
            if not path.is_file() or path.read_bytes() != data:
                changed.append(relative)
                if not arguments.check:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
    except OSError as error:
        if arguments.json:
            print(json.dumps({"ok": False, "error": str(error)}))
        else:
            print(f"Script fixtures: {error}", file=sys.stderr)
        return 1
    success = not arguments.check or not changed
    if arguments.json:
        print(json.dumps({"ok": success, "changed": changed}))
    else:
        print(f"Script fixtures: {len(changed)} {'stale' if arguments.check else 'written'} file(s)")
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
