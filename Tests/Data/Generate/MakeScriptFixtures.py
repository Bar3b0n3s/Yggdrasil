#!/usr/bin/env python3
"""Generate the authored script fixtures; --check never writes files and fails on stale or extra files.

Two output roots, each owned whole by this generator:
- Tests/Data/TypeChecker: the M13 type-checker fixtures and Expected.json, plus DocExamples/ (ADR 0022 decision 14):
  the stub modules and preludes the documentation's Luau examples are checked with, and the seeded document.
- Tests/Data/Scripts/Errors: the compile and type error fixtures and Expected.json (ADR 0022 decision 15).

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
ERRORS = ROOT / "Tests" / "Data" / "Scripts" / "Errors"

# The marker line that splits a doc-example prelude into the text before the example and the text after it. The whole
# prelude, marker included, is a script that type-checks on its own (it is listed Clean in Expected.json).
PRELUDE_MARKER = "-- @example"

# Stub modules the documentation's examples require, at their project paths (DocExamples/Assets/<path>).
DOC_EXAMPLE_STUBS: dict[str, str] = {
    "Scripts/Board.luau": """--!strict
-- A stub of the falling-block board module the documentation's examples require; only its types matter.
export type Board = {
\tWidth: number,
\tHeight: number,
\tCells: { string },
\tFillRows: (self: Board, first: number, last: number, gap: number) -> (),
\tPlace: (self: Board, piece: string, column: number, row: number, rotation: number) -> boolean,
\tClearLines: (self: Board) -> number,
\tToAscii: (self: Board) -> string,
}

local Board = {}

local function fillRows(self: Board, first: number, last: number, gap: number)
\tfor row = first, last do
\t\tfor column = 0, self.Width - 1 do
\t\t\tif column ~= gap then
\t\t\t\tself.Cells[row * self.Width + column + 1] = "#"
\t\t\tend
\t\tend
\tend
end

local function place(self: Board, piece: string, column: number, row: number, rotation: number): boolean
\tlocal index = row * self.Width + column + 1
\tif self.Cells[index] ~= "." or rotation < 0 then
\t\treturn false
\tend
\tself.Cells[index] = piece
\treturn true
end

local function clearLines(self: Board): number
\tlocal cleared = 0
\tfor row = 0, self.Height - 1 do
\t\tlocal full = true
\t\tfor column = 0, self.Width - 1 do
\t\t\tfull = full and self.Cells[row * self.Width + column + 1] ~= "."
\t\tend
\t\tif full then
\t\t\tcleared += 1
\t\tend
\tend
\treturn cleared
end

local function toAscii(self: Board): string
\treturn table.concat(self.Cells)
end

function Board.New(): Board
\tlocal board: Board = {
\t\tWidth = 10,
\t\tHeight = 22,
\t\tCells = table.create(220, "."),
\t\tFillRows = fillRows,
\t\tPlace = place,
\t\tClearLines = clearLines,
\t\tToAscii = toAscii,
\t}
\treturn board
end

function Board.Score(lines: number, level: number): number
\tlocal points = { 100, 300, 500, 800 }
\treturn (points[lines] or 0) * level
end

function Board.GravityTicksForLevel(level: number): number
\treturn math.max(1, 48 - level * 5)
end

return Board
""",
    "Scripts/Game.luau": """--!strict
-- A stub of the falling-block game behaviour the documentation's examples require; only its types matter.
local Board = require("./Board")

export type Game = ScriptInstance & {
\tStartLevel: number,
\tBoard: Board.Board,
\tLevel: number,
\tLockedCount: number,
\tGameOver: boolean,
}

local Game = {}

Game.Fields = {
\tStartLevel = Field.Integer(1, { Min = 1, Max = 20 }),
}

function Game.OnStart(self: Game)
\tself.Board = Board.New()
\tself.Level = self.StartLevel
\tself.LockedCount = 0
\tself.GameOver = false
end

return Script.Define("Game", Game)
""",
    "Scripts/Ball.luau": """--!strict
-- A stub of the rolling-ball behaviour the documentation's examples require; only its types matter.
export type Ball = ScriptInstance & {
\tTorque: number,
\tReachedGoal: boolean,
}

local Ball = {}

Ball.Fields = {
\tTorque = Field.Number(30, { Min = 0 }),
}

function Ball.OnStart(self: Ball)
\tself.ReachedGoal = false
end

return Script.Define("Ball", Ball)
""",
}

# Preludes (DocExamples/Preludes/<Name>.luau): an example with prelude=<Name> is checked as the text before the marker
# line, the example, then the text after it. Each is self-contained, so it checks alone wherever it is placed.
DOC_EXAMPLE_PRELUDES: dict[str, str] = {
    # A behaviour shell for examples that show one callback or method of a behaviour named Example.
    "Behaviour": f"""--!strict
export type Example = ScriptInstance & {{
\tSpeed: number,
\tCount: number,
}}

local Example = {{}}

Example.Fields = {{
\tSpeed = Field.Number(1, {{ Min = 0 }}),
}}

{PRELUDE_MARKER}

return Script.Define("Example", Example)
""",
    # The falling-block Game behaviour of Architecture §13.11, for its OnFixedUpdate excerpt. The methods are part of
    # the instance type, so strict mode checks self-method calls (`self:TryMove(0, -1)`).
    "TetrisGame": f"""--!strict
local Board = {{}}

function Board.GravityTicksForLevel(level: number): number
\treturn math.max(1, 48 - level * 5)
end

export type Game = ScriptInstance & {{
\tGameOver: boolean,
\tGravityTicks: number,
\tLevel: number,
\tReset: (self: Game) -> (),
\tHandleShift: (self: Game) -> (),
\tTryRotate: (self: Game, direction: number) -> boolean,
\tTryMove: (self: Game, columns: number, rows: number) -> boolean,
\tHardDrop: (self: Game) -> (),
\tBeginLockDelay: (self: Game) -> (),
}}

local Game = {{}}

function Game.Reset(self: Game)
\tself.GameOver = false
\tself.GravityTicks = 0
end

function Game.HandleShift(self: Game) end

function Game.TryRotate(self: Game, direction: number): boolean
\treturn direction ~= 0
end

function Game.TryMove(self: Game, columns: number, rows: number): boolean
\treturn columns ~= 0 or rows ~= 0
end

function Game.HardDrop(self: Game) end

function Game.BeginLockDelay(self: Game) end

{PRELUDE_MARKER}

return Script.Define("Game", Game)
""",
}

# The seeded document: three examples with a type error each (one wrapped by a prelude, one at its own path, and one
# whose mistake surfaces in the prelude's epilogue, after the example), a skipped example and a non-Luau block. The
# DocExamples test must report exactly those findings: the first two at their lines in the document, through the fence
# and prelude offsets, the third at its line in the prelude file.
SEEDED_DOCUMENT = """# Seeded documentation

The DocExamples test reads this page like a documentation page. Its three Luau examples have a type error on purpose.

```luau prelude=Behaviour
function Example.OnStart(self: Example)
\tself.Count = "three"
end
```

```luau path=Assets/Scripts/Seeded.luau
--!strict
local Seeded = {}

function Seeded.Count(): number
\treturn "seven"
end

return Seeded
```

```luau prelude=Behaviour
-- Shadows the engine's Script module, so the prelude's own return statement, after this example, indexes a number.
local Script = 5
```

```lua skip="shows invalid syntax on purpose"
local = 1
```

```json
{"Not": "Luau"}
```
"""

SEEDED_EXPECTED = {
    "Format": "DocExampleSeed",
    "Version": 1,
    "Document": "Seeded.md",
    "Checked": 3,
    "Skipped": 1,
    "Findings": [
        {"Line": 7, "Elsewhere": "", "Code": "SCRIPT_TYPE_ERROR", "MessageContains": "string"},
        {"Line": 16, "Elsewhere": "", "Code": "SCRIPT_TYPE_ERROR", "MessageContains": "string"},
        {"Line": 0, "Elsewhere": "Tests/Data/TypeChecker/DocExamples/Preludes/Behaviour.luau:15:8",
         "Code": "SCRIPT_TYPE_ERROR", "MessageContains": "number"},
    ],
}

# The compile and type error fixtures (Architecture §11.7, §15.5 "Errors.scene"; ADR 0022 decision 15), each checked
# through EditorScriptService::Write at Assets/Scripts/Errors/<File>, which runs the importer's compiler, load-time VM
# and type checker: File, Kind (compile or type), Code, Line, Column and a Message substring of the finding each must
# report.
ERROR_FIXTURES: dict[str, tuple[str, str, str, int, int, str]] = {
    "Syntax.luau": (
        "local Syntax = {}\n\nfunction Syntax.OnStart(self: ScriptInstance)\n\tlocal = 1\nend\n\n"
        "return Script.Define(\"Syntax\", Syntax)\n",
        "compile", "SCRIPT_COMPILE_ERROR", 4, 8, "Expected identifier",
    ),
    "UnresolvedRequire.luau": (
        "--!strict\nlocal Missing = require(\"./Missing\")\n\nreturn Missing\n",
        "compile", "SCRIPT_COMPILE_ERROR", 2, 1, "required script has no standalone asset",
    ),
    "EngineCallAtTop.luau": (
        "--!strict\nlocal player = Scene.FindByName(\"Player\")\n\nreturn { Player = player }\n",
        "compile", "SCRIPT_COMPILE_ERROR", 2, 1, "Scene.FindByName: is not available at load time",
    ),
    "RuntimeErrorAtTop.luau": (
        "--!strict\nlocal Settings = {}\n\nerror(\"module setup failed on purpose\")\n\nreturn Settings\n",
        "compile", "SCRIPT_COMPILE_ERROR", 4, 1, "module setup failed on purpose",
    ),
    "LoadTimeBudget.luau": (
        "--!strict\nlocal spins = 0\nwhile true do\n\tspins += 1\nend\n\nreturn spins\n",
        "compile", "SCRIPT_COMPILE_ERROR", 3, 1, "exceeded 250 ms",
    ),
    "TypeMismatch.luau": (
        "--!strict\nlocal count: number = \"three\"\n\nreturn count\n",
        "type", "SCRIPT_TYPE_ERROR", 2, 23, "Expected this to be 'number', but got 'string'",
    ),
    # The behaviour EditorPlayController's reload type gate rewrites a running script with (ADR 0022 decision 15).
    "TypeReload.luau": (
        "--!strict\nlocal TypeReload = {}\n\nfunction TypeReload.Current(self: ScriptInstance): number\n"
        "\treturn \"two\"\nend\n\nreturn Script.Define(\"TypeReload\", TypeReload)\n",
        "type", "SCRIPT_TYPE_ERROR", 5, 9, "number",
    ),
}

# Every seeded mistake pins its exact findings: (code, (line, column, endLine, endColumn), message substring), in the
# checker's (File, Line, Column, Code) order. A fixture failing for an unrelated reason cannot match these.
DIAGNOSTICS: dict[str, list[tuple[str, tuple[int, int, int, int], str]]] = {
    "Errors/ComponentOverload.luau": [
        ("SCRIPT_TYPE_ERROR", (3, 2, 3, 41), "Expected this to be 'Transform?', but got 'RigidBody?'"),
    ],
    "Errors/GenericMethod.luau": [
        ("SCRIPT_TYPE_ERROR", (3, 2, 3, 33), "Expected this to be 'string', but got 'number'"),
    ],
    "Errors/Optional.luau": [
        ("SCRIPT_TYPE_ERROR", (2, 1, 5, 4), "Consider annotating the return with number"),
        ("SCRIPT_TYPE_ERROR", (2, 1, 5, 4),
         "Operator '-' could not be applied to operands of types vector? and vector?"),
        ("SCRIPT_TYPE_ERROR", (4, 9, 4, 19),
         "Operator '-' could not be applied to operands of types vector? and vector?"),
    ],
    # §11.9's seeded mistake: an optional passed to a non-optional parameter.
    "Errors/OptionalArgument.luau": [
        ("SCRIPT_TYPE_ERROR", (7, 16, 7, 19), "Expected this to be 'vector', but got 'vector?'"),
    ],
    "Errors/Sealed.luau": [
        ("SCRIPT_TYPE_ERROR", (6, 1, 6, 12), "Cannot add property 'Added' to table"),
    ],
    "Errors/Type.luau": [
        ("SCRIPT_TYPE_ERROR", (1, 23, 1, 28), "Expected this to be 'number', but got 'string'"),
    ],
}


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
    sources["Errors/OptionalArgument.luau"] = '''--!strict
local function length(value: vector): number
\treturn vector.magnitude(value)
end
local function bad(entity: Entity): number
\tlocal low = entity:GetWorldBounds()
\treturn length(low)
end
return bad
'''
    sources.update(doc_example_sources())
    expectations = []
    for path in sorted(sources):
        expected: dict[str, object] = {"Path": path, "Clean": not path.startswith("Errors/")}
        if path in DIAGNOSTICS:
            expected["Diagnostics"] = [
                {"Code": code, "File": f"Assets/{path}", "Range": list(extent), "MessageContains": message}
                for code, extent, message in DIAGNOSTICS[path]
            ]
        expectations.append(expected)
    sources["Expected.json"] = json.dumps(expectations, indent="\t", ensure_ascii=False) + "\n"
    return sources


def doc_example_sources() -> dict[str, str]:
    """The DocExamples stubs and preludes, relative to Tests/Data/TypeChecker; each checks clean on its own."""
    sources = {f"DocExamples/Assets/{relative}": source for relative, source in DOC_EXAMPLE_STUBS.items()}
    for name, source in DOC_EXAMPLE_PRELUDES.items():
        if source.count(PRELUDE_MARKER + "\n") != 1:
            raise ValueError(f"prelude {name} needs exactly one '{PRELUDE_MARKER}' line")
        sources[f"DocExamples/Preludes/{name}.luau"] = source
    return sources


def doc_example_documents() -> dict[str, str]:
    """The seeded document and its expectation, relative to Tests/Data/TypeChecker."""
    return {
        "DocExamples/Seeded/Seeded.md": SEEDED_DOCUMENT,
        "DocExamples/Seeded/Expected.json": json.dumps(SEEDED_EXPECTED, indent="\t", ensure_ascii=False) + "\n",
    }


def error_fixtures() -> dict[str, str]:
    """The compile and type error fixtures and their Expected.json, relative to Tests/Data/Scripts/Errors."""
    sources: dict[str, str] = {}
    expectations = []
    for name in sorted(ERROR_FIXTURES):
        source, kind, code, line, column, message = ERROR_FIXTURES[name]
        sources[name] = source
        expectations.append({"File": name, "Kind": kind, "Code": code, "Line": line, "Column": column,
                             "Message": message})
    sources["Expected.json"] = json.dumps(expectations, indent="\t", ensure_ascii=False) + "\n"
    return sources


def owned_files(root: Path) -> set[str]:
    """Every file below `root`, '/'-separated and relative to it; the generator owns the whole directory."""
    if not root.is_dir():
        return set()
    return {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()}


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Exit codes: 0 fixtures generated or verified, 1 stale fixtures or I/O failure, 2 usage error.",
    )
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--json", action="store_true")
    arguments = parser.parse_args()
    changed: list[str] = []
    extra: list[str] = []
    try:
        type_checker = fixtures()
        type_checker.update(doc_example_documents())
        for root, files in ((OUTPUT, type_checker), (ERRORS, error_fixtures())):
            label = root.relative_to(ROOT).as_posix()
            for relative, source in files.items():
                path = root / relative
                data = source.encode("utf-8")
                if not path.is_file() or path.read_bytes() != data:
                    changed.append(f"{label}/{relative}")
                    if not arguments.check:
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(data)
            for relative in sorted(owned_files(root) - files.keys()):
                extra.append(f"{label}/{relative}")
                if not arguments.check:
                    (root / relative).unlink()
    except (OSError, ValueError) as error:
        if arguments.json:
            print(json.dumps({"ok": False, "error": str(error)}))
        else:
            print(f"Script fixtures: {error}", file=sys.stderr)
        return 1
    success = not arguments.check or not (changed or extra)
    if arguments.json:
        print(json.dumps({"ok": success, "changed": changed, "extra": extra}))
    else:
        verb = "stale" if arguments.check else "written"
        removed = "extra" if arguments.check else "removed"
        print(f"Script fixtures: {len(changed)} {verb} file(s), {len(extra)} {removed} file(s)")
        for name in changed + extra:
            print(f"  {name}")
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
