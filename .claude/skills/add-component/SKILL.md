---
name: add-component
description: Use this skill whenever you add a built-in component or change one (a new field, a renamed or retyped field, a new enum value, a flag, a requirement), so its header, registration, consuming systems, migration with its format golden, validation codes and fixtures, generated references, script proxy and FeatureTest coverage stay consistent, and replays and demo projects follow the change.
---

# Add or change a component

Components are plain data structs reflected once in the type registry (Architecture §5.3, §5.4). Serialization, the inspector, automation (`entity.*`, `component.schema`), script proxies (§11.4), the JSON Schemas, `Docs/Reference/Components.md` and the coverage gates are all generated from that one registration, so a component needs no hand-written JSON, UI or binding code. Read §5.3 to §5.5, §6 (file rules and migrations) and §15.6 (gates) first.

## Files to touch

1. **The header** `Engine/Source/Engine/Scene/Components/<Name>Component.h`: a plain struct, every member default-initialized, no behaviour beyond trivial helpers. Members are declared in the order the file's keys will have. Use the reflected field types (`float`, `uint32_t`, `glm::vec3`, `glm::quat`, `std::string`, `UUID` for an entity reference, `TypedAssetHandle<AssetType::...>` for an asset, a registered `enum class`, `std::vector`/`std::map<std::string, T>` of those, a registered struct, `VariantValue`). A default must be valid: the registry suite creates defaults and checks them.
2. **`Engine/Source/Engine/Scene/Components/BuiltinComponents.h`**: add the type to `BuiltinComponents` at its category's position. Its position is the canonical component order of every file, so append within the category. This is a shared integration file with one owner per milestone: route the change through its owner and list it in your result.
3. **The category registration** `Engine/Source/Engine/Scene/Registration/<Category>Registration.cpp`, in `BuiltinComponents` order:
   - `RegisterComponent<T>(registry, "<Name>", "<one-sentence description>")` with `.Category("<Category>")` and `.Version(1)`.
   - `.Flags(...)` or `.RemoveFlags(...)` when the defaults (`Serializable | ScriptVisible | EditorVisible | Removable`) do not fit: `UniquePerScene` (one per scene, like `PostProcess`), `NoShortcut`, `Hidden`, `Required`, `EntityLevel`.
   - `.Requires<Other>()` / `.Excludes<Other>()` relations (`COMPONENT_MISSING_REQUIREMENT`, `COMPONENT_CONFLICT`).
   - `.Field("Name", &T::Member, "description", { .Min, .Max, .Step, .MinMagnitude, .Unit, .ReadOnly, .Scriptable, .Modes })` per member, with ranges on every numeric field that has one, `.ColorField` for colours, `.VirtualField` for computed values, `.Validate(...)` plus `.Generate(...)` for cross-field rules. **Every type, field and enum value needs a non-empty description**: the registry asserts it and gate 7 checks it.
   - Register a new enum with `registry.Enum<E>("Name", "description").Entry(E::Value, "Value", "description")` before the component that uses it; each value has a description.
4. **The consuming systems.** Read the component where it is used (renderer extraction, `PhysicsSystem`, `AudioSystem`, `ScriptEngine`, ...). Systems read components; they never store references to them across structural changes.
5. **Migrations.** Changing a serialized field of an existing component (rename, retype, removal, a changed meaning) bumps `.Version(N + 1)` and registers `.Migration(N, &Upgrade...)`, a pure `json -> json` function of one component object. Add a golden fixture in `Tests/Data/Formats/` (the old file and the migrated result) so old files keep loading. Adding a field with a default needs no migration, but it changes the canonical form (see "Consequences").
   - **Prefab overrides follow the component.** An instance's `Prefab` component names fields by their registry names (a Field override's `Component` and `Field`, an AddComponent override's whole value), so a migration that moves or reshapes a field also rewrites, in the same upgrade, every override that names the old field, whatever the Prefab component's version (`Engine/Source/Engine/Scene/Migrations.cpp`). Script version 2 is the example: it wrapped `Script`, `Fields` and `ExecutionOrder` into the first slot of the list `Scripts`, and its migration turned a Field override of `Fields` into one of `Scripts/0/Fields` and an AddComponent value into a one-slot list (ADR 0031 decision 17). The golden fixture holds such overrides.
   - **List fields and prefab overrides.** The prefab diff records a changed list field as one override of the whole list. A list whose elements need overrides of their own (each element a separately edited thing) needs element-addressed override fields and their own diff and apply rules, as the Script component's per-slot fields `Scripts/<slot>/<member>` (`PrefabInstantiator::ParseScriptSlotField` and `MakeScriptSlotField`, with the override resolver's value schemas in `CoreRegistration.cpp`; ADR 0031 decision 19). Index-addressed overrides follow an element's index, which a reorder in the prefab moves: say so where the component is documented.
6. **Validation.** A rule the type registry cannot express (a cross-component or scene-level condition) becomes a validation code in `Editor/Source/EditorCore/Project/ProjectValidator.cpp`: add the code to `ValidatorCodes` and its documentation to `ValidatorCodeInfos` (severity, auto-fix, one-sentence description and fix), list it in Architecture §13.7, add a fixture `Tests/Data/Validation/<CODE>/` through `Tests/Data/Generate/MakeValidationFixtures.py` (a copy of the `Clean` baseline that triggers exactly the new code), and add the code's row to the `game-debugging` skill's validation table.
7. **Tests.** The registry suite covers round trip, random values, schemas and defaults with no extra code. Add unit tests for what the component does in its systems and for every validation rule, including the error paths.

## Generated files to refresh

```
python Scripts/Build.py --config Debug
python Scripts/GenerateDocs.py --config Debug
```

`GenerateDocs.py` regenerates `Docs/Reference/Components.md` (fields, defaults, ranges, enums, script members), `Docs/Reference/Schemas/*.schema.json` (the scene and prefab schemas embed every component), `ScriptAPI.md`, `Resources/Scripting/Engine.d.luau` (the proxy type and the `GetComponent` overload) and, when automation types change, `Methods.json` and `Tools/MCP/catalog.json`. `--check` must be clean before a commit; commit the regenerated files with the change.

## FeatureTest coverage

Every new component, field and enum value is covered in `Projects/FeatureTest` in every run mode (Roadmap rule 5, §15.5, §15.6), through the project's scaffolds (`Projects/FeatureTest/Scaffold/*.jsonl`, regenerated with `python Scripts/Scaffold.py`; never edit project files by hand):
- **Gate 1:** `Components.scene` (or a variant scene under `Assets/Scenes/Components/` for a component that is unique per scene) holds the component with **every serialized field set to a non-default value** somewhere. Fields are counted per leaf of the component's JSON, every array element and map value written `*` (`field:Script/Scripts/*/ExecutionOrder`), so each member of a list of structs needs a non-default value in some element.
- **Gate 1b:** every value of every enum the component's fields use occurs in some simulated scene.
- **Gate 4:** a suite under `Assets/Tests/Api/` reads and writes every scriptable field through the script proxy (`entity.<Name>.<Field>`); read-only fields are read.
- Run `python Scripts/Test.py --suite feature --config Debug --require-gpu`: it fails with `UNCOVERED gate <g> (Editor): <id>` lines naming each missing field, value or proxy access.

## Consequences of a new serialized field

- **State hashes change.** The state hash covers serialized component data, so every committed replay that touches the component's scenes changes its checkpoints and `FinalStateHash`. Re-record the replays (which rewrites both) through automation (`game-building`, "Re-recording replays") and state in the review why the hashes changed; `Projects/FeatureTest`'s replays are recorded by `Scaffold/Replays.jsonl`.
- **The old shape is refused.** Automation and script values take only the current shape (no compatibility shim): an old field name is an unknown field, with a hint naming its replacement where the change gives one (Script version 2's `Scripts`). Rewrite every scaffold, skill, test, fixture and template that writes the old shape in the same change.
- **Canonical form changes.** Files written before the change lack the new key, so the byte-identical re-save test fails for every project that holds the component. Run `project.upgrade` through automation on each demo project (`Editor --headless --project <Game> --upgrade`, which transcribes `project.refreshAssets` and `project.upgrade` as client `cli`, or `project.upgrade` through the bridge), which rewrites the files and records provenance and their transcript lines, and regenerate `Projects/FeatureTest` with `Scaffold.py`.
- **Goldens** may change when the field affects rendering (`golden-images`).

## Checklist

- [ ] Header, `BuiltinComponents.h` (through its owner), registration with category, flags, ranges, units and descriptions.
- [ ] Consuming system and its tests; validation codes with fixtures, `ValidatorCodeInfos` and the `game-debugging` table.
- [ ] Migration and format golden for a changed field.
- [ ] `GenerateDocs.py --check` clean with the regenerated references committed.
- [ ] FeatureTest covers gates 1, 1b and 4 in every mode; the feature suite is green.
- [ ] Replays re-recorded and demo projects upgraded, with the reason recorded.
