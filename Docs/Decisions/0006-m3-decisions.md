# 0006 — M3 contract decisions

- **Status:** proposed by the M3 contract task and revised after two reviews of the contract, whose accepted findings are folded into decisions 1 to 20. The streams implemented against it; the M3 integration task resolved what it left open (decisions 21 to 31), the review of the integrated milestone added decisions 32 to 37, and the docs owner applies the amendments listed at the end of each part.
- **Date:** 2026-10-06
- **Context:** The M3 contract task (Roadmap rule 3) froze the public headers of Reflection, Scene, the scene and project formats and the minimal Asset types components need. Writing complete headers exposed places where the Architecture is silent, sketches an API that cannot compile as written, or conflicts with the layer rules of §3. `AGENTS.md` ("Deviations") requires a record of each departure.

## Decisions

### 1. Reflection reaches the Scene only through two opaque names

§5.4 puts type-erased `Add/Remove/Has/Patch` on `ComponentInfo` and has virtual fields such as `Transform.WorldPosition` walk the parent chain, but Reflection (layer 1) may include neither EnTT nor Scene (layer 3).

- **ECS operations:** `Reflection/ComponentInfo.h` forward-declares `struct ComponentHostOps` and only stores a pointer to it. `Scene/ComponentHostOps.h` defines it (function pointers taking an `Entity`, or a `ConstEntity` for the read-only `Has` and `GetConst`, by value), and `Scene/ComponentRegistration.h` provides `RegisterComponent<T>(registry, name, description)`, which calls `registry.Component<T>(...)` and installs the constant `Detail::ComponentHostOpsFor<T>`. Every built-in component is registered through it, so the §5.4 example reads `RegisterComponent<RigidBodyComponent>(registry, "RigidBody", ...)` instead of `registry.Component<RigidBodyComponent>(...)`.
- **Virtual fields:** `FieldContext {void* Object; const Entity* Owner}` forward-declares `class Entity`; Reflection passes the pointer through and never dereferences it.
- **Rejected:** a `void*` scene handle (loses type safety), a Scene-side table keyed by component index (a second registry to keep in sync), and a customization-point template that Reflection instantiates but Scene defines (it cannot be partially specialized for every T).

### 2. Asset handles before the M6 Asset module

Components need asset references now. §5.3 says "`AssetRef<T>` fields hold an `AssetHandle`", while §4.7 and §7.2 define `AssetRef<T> = Ref<const T>`, the loaded asset. One name cannot be both.

- `Asset/AssetHandle.h`: `using AssetHandle = UUID;` (§7.1 "AssetHandle is a UUID").
- `Asset/AssetType.h`: `enum class AssetType : uint16_t { None = 0, Scene, Prefab, Mesh, Material, Texture, Environment, AudioClip, Script, Font, Replay }` with constexpr `AssetTypeToString`/`AssetTypeFromString`. The values are persisted (§6.8 cooked header) and only ever appended to. `None` is the "any type" filter: `TypedAssetHandle<AssetType::None>` gives the empty `AssetFilter`, which means any type.
- `Asset/TypedAssetHandle.h`: `TypedAssetHandle<AssetType Type>`, the storage of an asset-reference field. It is parametrized on the `AssetType` value, so no asset class has to exist yet, and a mesh slot cannot be assigned a material handle in C++.
- M6 adds `AssetRef<T> = Ref<const T>`, the metadata, the registry and the manager next to these headers, and changes none of them.
- Reflection recognizes `TypedAssetHandle` through `AssetFieldTraits<T>`, a customization point declared in `Reflection/TypeInfo.h` and specialized in `TypedAssetHandle.h` (Asset may include Reflection; the reverse is forbidden).

### 3. `FieldMeta::AssetFilter` is the asset type's name

§5.4 writes `AssetType AssetFilter = AssetType::None`, but `FieldMeta` lives in Reflection, which cannot include `AssetType`. `AssetFilter` is a `std::string` holding the `AssetType` name ("Mesh"; empty means any), which is also how scripts (`Field.Asset("AudioClip")`), `.meta` files and automation spell asset types. The registry fills it from the field's `TypedAssetHandle` type; registrations never write it. `Unit` and the other `FieldMeta` strings are `std::string` too, because script field schemas (M13) create `FieldInfo`s at run time and must own their text. Every `FieldMeta` member has a default member initializer, so a partial designated initializer such as `{ .Min = 0.001, .Unit = "kg" }` does not trip Clang's `-Wmissing-field-initializers`.

### 4. `Project` is a layer-2 module

Roadmap M3 names `Project/ProjectSettings` and `ProjectSerializer`, but §3 has no Project module. `Engine/Source/Engine/Project/` is added to `Scripts/ModuleRules.json` at layer 2, including Core, Reflection and Platform. It sits beside Asset because every layer above consumes the settings, and Platform is allowed so that input control names can later be validated against Platform's key tables. Scene does not include it.

### 5. The reflection data model

The Architecture names `TypeInfo`, `FieldInfo` and `Value` without defining them.

- **Types:** `TypeInfo` describes one value type: its `FieldType`, element type (Array, Map), `EnumInfo`, `StructInfo`, the asset type name of an AssetRef, and `TypeOps`, type-erased operations generated by `Detail::MakeTypeOps<T>()` (create, copy, reset, scalar read/write through `Value`, container access). A schema-only `TypeInfo` has no operations; it describes JSON only, for script field schemas.
- **In-process identity:** `TypeKey` is the address of a per-type constant (`TypeKeyOf<T>()`), because first-party code never uses `typeid`.
- **Fields:** `FieldInfo` is stored (a `MemberFieldAccessor`), virtual (getter and optional setter) or schema-only. It holds its `Specification` as a whole.
- **Structs:** `StructInfo` owns ordered fields and validators and implements the generic `ToJson`, `FromJson` (atomic, unknown members warn), `ApplyMergePatch` (RFC 7386) and `Validate`. `ComponentInfo` derives from it.
- **Merge patches:** `ApplyMergePatch` equals reading `MergePatch(ToJson(object), patch)` into a fresh `CreateDefault()` object, so a null member resets a field to its default. One exception to RFC 7386 nesting: a member that targets a Variant field, or a value of a Map of Variants, replaces it whole, because a Variant's shape belongs to its resolved schema.
- **Generate hooks:** field metadata cannot express type-level rules (unique layers whose first is "Default", a spot light's inner cone below its outer one, array element ranges). Every type with a `Validate` rule therefore also registers a `Generate` hook (`FieldBuilder::Generate(void (*)(T&, Random&))`, asserted by `Freeze`). `RandomValueGenerator::Randomize` runs the hooks bottom-up after its field pass, so the registry suite's 200 random values per type pass `Validate` and `FromJson`, not only the field checks.
- **Values:** `Value` is a tagged value, so Vec3 and Color3 differ, and so do EntityRef and AssetRef. It stores an enum as its integer, a Map as sorted keys plus values, a Struct as field names plus values, and a Variant as a `VariantValue`. Its kind accessor is `GetKind()`, like `TypeInfo` and `FieldInfo`; `GetType()` always returns a `TypeInfo`.
- **Variants:** `VariantValue`, the storage of Variant fields, holds one JSON value as shared immutable data (`Ref<const Json>`, §4.7).
- **Color fields:** a `glm::vec3`/`glm::vec4` member reflects as Vec3/Vec4, and as Color3/Color4 through `FieldBuilder::ColorField`.
- **Plain UUIDs:** members of type `UUID` reflect as EntityRef (`using EntityRef = UUID`).
- **Other names fixed here:** the enum builder method is `Entry` (a member called `Value` would shadow the type `Value`), and the builders are CRTP classes declared after `TypeRegistry` so Clang sees a complete registry. `TypeRegistry::AddComponent/AddStruct/AddEnum/AddType` are private: only the registry's own templates register types.

### 6. Variant resolution context, schema sources and free-form variants

§5.4 types the resolver as `Result<const FieldInfo*> (*)(const ResolveContext&)` but does not say what the context holds, or where script field schemas come from before M13.

- **Context:** `ResolveContext {Registry, Owner, OwnerType, OwnerJson, Key, Schemas}`. Owner is the C++ object holding the Variant field, or null when a value is checked without one (`FieldInfo::ValidateJson`). OwnerType is the owner's registered type, and every resolver checks it before reading the owner, returning InvalidArgument for another type, so a resolver can never misread, say, a `PrefabOverride` as a `ScriptComponent`. OwnerJson is the owner's JSON object when the value comes from JSON; a resolver reads it when Owner is null (the Script handle of an AddComponent override's Script object). Readers read every field declared before the Variant field first, so `ScriptComponent::Script` and `PrefabOverride::{Kind, Component, Field}` are available to the resolver.
- **Descent:** validating a resolved schema that is itself a struct replaces Owner, OwnerType and OwnerJson with the nested object and clears Key. A Field override of `Script.Fields` resolves to the field without its component, so the script field resolver gets OwnerType Script and no owner: those values are kept with `REFLECTION_VARIANT_UNRESOLVED` until `PrefabInstantiator` applies them through `ComponentAccess`, where the member's own `ScriptComponent` is the owner.
- **Schema source:** `IFieldSchemaSource` (`FindField(owner, name)`, `GetFieldNames(owner)`) is how script field schemas reach the resolver in M13. The tests use `Test::FixtureSchemaSource`, the "fixture resolvers" of Roadmap M3.
- **Reads vs writes:** while reading, an unresolvable Variant value (`REFLECTION_VARIANT_UNRESOLVED`) or one that fails its resolved schema (`REFLECTION_VARIANT_MISMATCH`) is kept verbatim and reported as a warning (an error with `--strict`). That matches §11.2: "fall back ... and are preserved in the file until fixed". Writes through the reflected setters reject an invalid value.
- **Free-form variants:** a Variant field without a resolver holds free-form JSON. Its one user is `TestSuiteSettings::Parameters` (the §11.10 `Parameters`), which is a new Variant use beyond §5.4's list. The registry suite covers it as §5.4 requires.

### 7. Entity-level state

- **`DisabledTag` is not reflected.** It is the entity key `"Active"` and is set through `Entity::SetActive`. A zero-field tag has no fields to reflect, and EnTT stores empty types without instances.
- **`EntityLevel` also covers ID and Relationship.** Both are reached through Entity members (`.ID`, `GetParent`/`GetChildren`, §11.5), so `EntityLevel` means "serialized as entity keys": ID → `"ID"`, Name → `"Name"`, Relationship → `"Parent"`, Tags → `"Tags"`.
- **Tags on every entity.** Every entity has `TagsComponent` (an empty list by default), so `"Tags"` is always written, per "explicit beats implicit".
- **Tracking and revisions.** The scene's change hooks receive the component's `TypeKey`. Changes to a type that is neither registered nor `DisabledTag` (the runtime-only components of decision 8) neither increment the revision nor reach the change tracker, so `TransformSystem::Update` adding `WorldTransformComponent` in an edit scene causes no spurious `ifRevision` conflicts. `DisabledTag` changes are recorded as the entity key `"Active"`.
- **Key order:** entity keys are written in the order `ID, Name, Parent, Active, Tags, Components`, as in §6.2.

### 8. Runtime-only components

M3 defines the runtime components the scene itself maintains (`Components/RuntimeComponents.h`): `WorldTransformComponent`, `PreviousWorldTransformComponent`, `InterpolationResetTag`, `HierarchyDisabledTag`, `PendingDestroyTag` and `PendingStartTag`. `PhysicsBodyRuntime`, `CharacterRuntime`, `AudioSourceRuntime` and `ScriptRuntime` hold Jolt, miniaudio and Luau state, so M11, M12 and M13 define them. Declaring empty placeholders now would be invented content.

### 9. Built-in registration table

Every component is version 1, and every field is registered in member declaration order. Defaults are the §5.3 values in the component headers. `Default` means `Serializable | ScriptVisible | EditorVisible | Removable`.

| Category file | Component | Flags beyond Default | Relations and field metadata |
|---|---|---|---|
| Core | `ID` | Required, Hidden, EntityLevel; not Removable | `ID` read-only |
| Core | `Name` | Required, EntityLevel; not Removable | — |
| Core | `Tags` | EntityLevel; not Removable | — |
| Core | `Relationship` | Required, Hidden, EntityLevel; not Removable | fields read-only |
| Core | `Transform` | Required; not Removable | `Translation` (m); `Scale` MinMagnitude 1e-4; virtual `EulerAngles` (deg, rw), `WorldPosition` (m, rw), `WorldRotation` (rw), `WorldScale` (ro), `RenderPosition` (ro), `RenderRotation` (ro) |
| Core | `Prefab` | Hidden; not EditorVisible, not Removable | structs `PrefabEntityKeys` and `PrefabOverride` with enum `PrefabOverrideKind`; `Value` is a VariantField with the override resolver (field schema for Field, `GetSelfField()` of the component for AddComponent, the `PrefabEntityKeys` field for EntityKey) |
| Core | `PrefabLink` | Hidden; not EditorVisible, not Removable | — |
| Rendering | `MeshRenderer`, `Camera`, `DirectionalLight` (CascadeCount 1–4), `PointLight`, `SpotLight`, `Text` | — | colours are ColorFields; angles in deg, distances in m; SpotLight validates inner cone < outer cone, with its Generate hook |
| Rendering | `Environment` (SkyboxBlur 0–1), `PostProcess` | UniquePerScene | — |
| Physics | `RigidBody` | — | Requires Transform, Excludes CharacterController; Mass Min 0.001 kg; `EnhancedInternalEdgeRemoval` |
| Physics | `BoxCollider`, `SphereCollider`, `CapsuleCollider` | — | Requires Transform; HalfExtents, Radius and HalfHeight Min `MinColliderDimension` (0.001 m) |
| Physics | `MeshCollider` | — | Requires Transform |
| Physics | `CharacterController` | — | Requires Transform, Excludes RigidBody |
| Audio | `AudioSource`, `AudioListener` | — | — |
| Scripting | `Script` | NoShortcut | `Fields` is a VariantField whose resolver looks each key up through `ResolveContext::Schemas->FindField(Script handle, key)`, the handle read from Owner or, without one, from `OwnerJson["Script"]` |

Hidden components are engine-maintained: `ComponentAccess` reads them but rejects every write to them (add, remove, set, patch) with InvalidArgument, so automation cannot create an inconsistent prefab link; only `PrefabInstantiator` adds and removes `Prefab` and `PrefabLink`.

Enums are registered by their C++ names, with the enumerator names as entries: `ProjectionType`, `ClearMode`, `Tonemapper`, `SsaoQuality`, `TextSpace`, `TextAlignment`, `BodyType`, `MotionQuality`, `Attenuation`, `AudioGroup`, `PrefabOverrideKind`. Members that share their enum's name (`SsaoQuality`, `MotionQuality`, `Attenuation`) spell the type `Engine::X`. Otherwise GCC's "changes meaning" rule rejects the struct.

### 10. Scene API refinements

- **`SetParent` returns `Status`.** §5.2 rejects cycles with InvalidArgument, which a `void` function (the §5.1 sketch) cannot report.
- **Overloads instead of `Entity parent = {}`.** `CreateEntity(name)`/`CreateEntity(name, parent)` and the `CreateEntityWithID` pair replace the default argument, because `Entity` is incomplete in `Scene.h`: `Entity.h` includes `Scene.h` for its component templates.
- **`ForEachCanonical` is defined in `Entity.h`.** It is declared in `Scene.h`; its callback takes an entity handle, so callers include `Entity.h` anyway.
- **Const access through `ConstEntity`.** `Entity` holds a mutable `Scene*`, so a const member returning it could only be written with a `const_cast`, which CodeStyle §8 forbids, and would let a `const Scene&` holder mutate the scene. `ConstEntity {entt::entity, const Scene*}` is the read-only handle: the const overloads of `FindEntityByID`, `FindEntityByPath`, `ResolveEntityPath` and `ForEachCanonical` return it, and read-only code takes it (`SceneSerializer::EntityToJson`, `ComponentAccess::GetComponentJson`, `Prefab::CreateFromEntity`, `ComponentHostOps::Has/GetConst`, the `TransformSystem` getters, `GetEntityPath`). `Entity` converts to it implicitly. Both handles are passed by value (CodeStyle §6); `FieldContext::Owner` stays `const Entity*`, because Reflection cannot see the complete type.
- **Additions:** `ResolveEntityPath` (located errors and candidate lists for automation), `GetEntityPath`, `FlushPendingDestroys` (§5.7 step 8), `Clear`, and the frame's interpolation Alpha (`Get/SetInterpolationAlpha`, read by `RenderPosition`).
- **Entity paths:** `[n]` is the zero-based index among same-named siblings; a path segment without an index must match exactly one sibling. Names are unrestricted, so paths escape `\`, `/`, `[` and `]` in names with a backslash ("A/B" is `/A\/B`); a backslash before any other character is malformed. `GetEntityPath` always produces a path `FindEntityByPath` resolves back to the entity, which automation (§13.4) relies on.
- **Reparenting:** `SetParent(..., keepWorld = true)` returns InvalidArgument with nothing changed when the resulting local transform is not representable (decomposition fails, or a scale component falls below `MinTransformScaleMagnitude`, which `Transform.Scale`'s MinMagnitude would reject on the next load).
- **Asserts:** `CreateEntityWithID` asserts with messages containing "needs a valid ID" and "is already used"; the death tests check those substrings.
- **Serializer-backed members:** `ComputeStateHash` is XXH64 (seed 0) of the minified canonical serialization, implemented in `Scene/SceneStateHash.cpp`. The change tracker's "before" snapshot is the private `Scene::CaptureEntitySnapshot`, implemented in `Scene/EntitySnapshot.cpp`. Stream C owns both files, so `Scene.cpp` (stream B) never depends on serializer internals.
- **Prefab instantiation by handle:** `Scene::InstantiatePrefab(handle, ...)` needs the AssetManager (M6). M3 provides `PrefabInstantiator::Instantiate(scene, prefab, options)`, and M6 adds the handle-resolving wrapper.

### 11. Settings without optional fields

The registry has no optional `FieldType`, but §11.10 has optional suite keys. Sentinels stand in for them, documented on each field:

- `TestSuiteOverrides`: 0 means "use the project value" for `CallbackBudgetMs` and `MemoryLimitMB`; `PauseOnError` is an enum `Inherit`/`Pause`/`Continue`.
- `ExpectQuit`: -1 means "must not quit".
- An empty `Scene` means "an empty scene"; an empty `Clock` means "lockstep ManualClock".

Because "default-valued fields are written", canonical files list these keys explicitly.

Other settings decisions:

- **Nested enums:** the settings enums are nested in their structs (`InputActionSettings::ActionType`, registry name `InputActionType`; `TestSuiteSettings::Mode` and `IsolationMode`; `TestSuiteOverrides::PauseOnErrorOverride`), so they cannot collide with M2's Platform input types.
- **Defaults** where §6.1 gives none: `Name` "Untitled", `Window.Title` "Game", `Seed` 0, `Layers` ["Default"], `Collisions` [["Default", "Default"]], `Export.Exclude` ["Assets/Tests/**"], `Export.Version` "1.0.0".
- **Validation rules** where §6.1 gives none: 1 to 32 unique layers, the first one "Default"; collision pairs of declared layers; `ShadowMapSize` a power of two in [256, 8192]; `CallbackBudgetMs` >= 10; `FixedHz` in [1, 100000] (ADR 0003 decision 8); `Export.Version` in the form `major.minor.patch`. Each validated settings struct registers the Generate hook (decision 5) that turns random values into valid settings.

### 12. Version 0 of the entity format

Roadmap M3 asks for "Migrations: v0 fixture upgrades to v1", but no version 0 was ever defined. Version 0 is the pre-release entity format:

- entities carry `"Enabled"` instead of `"Active"`;
- `"Components"` is an array of `{"Type": <registry name>, <fields>...}` objects instead of an object keyed by name;
- there is no `"ComponentVersions"` (every component is at version 1).

`Migrations::UpgradeVersion0To1` renames the key, keys the components by type (keeping array order), adds `ComponentVersions` and sets `Version` to 1. A type listed twice on one entity is a located Validation error. The golden pair is `Tests/Data/Formats/Scene/v0.scene`, whose load and save gives `v0.upgraded.scene` byte-identically.

### 13. Fixtures and how the Tests binary finds them

This resolves ADR 0003 decision 27.

- **Locating Tests/Data:** `Tests/Source/Support/TestData.h` reads `ENGINE_REPO_ROOT`, which `ApplyFirstPartySettings` already defines for every non-Dist configuration. It provides `GetTestDataPath`, `ReadTestDataText` and `ListTestDataFiles` (the list is sorted, so test order never depends on the file system).
- **Layout:**
  - `Tests/Data/Scenes/*.scene`: the canonical fixtures that must re-save byte-identically.
  - `Tests/Data/Scenes/Invalid/`: one fixture per structural defect. These are exempt from the re-save test, because strict loading rejects them by design.
  - `Tests/Data/Formats/Scene/`: migration goldens and the newer-version file.
  - `Tests/Data/Prefabs/Block.prefab`.
  - `Tests/Data/Project/AllSettings.eproj`.
- **Canonical JSON check:** `AuthoredFiles: every authored file under Tests/Data is canonical JSON` already runs, against M1's reader and writer. It is the directory-wide half of §6's load → save rule. `Projects/` joins it when that directory first exists (M14).
- **Prefab-derived IDs:** the IDs in `AllComponents.scene` are real `Hash64(root, prefabEntityID)` values, and the prefab tests cross-check one of them.

### 14. Unknown components and `ComponentVersions`

- **Storage:** an entity's unknown components are kept in `UnknownComponentsComponent`, a Scene type that is not reflected. Each entry holds the component's name, its `ComponentVersions` entry and its JSON verbatim. The component travels with every serializer copy: the play copy, undo snapshots and prefab instantiation.
- **Writing:** unknown components are written after the known ones in their original order, and their version entries likewise, so a file whose unknown components come last re-saves byte-identically.
- **`ComponentVersions`:** lists every component type present in the document, known ones in registry order. EntityLevel components are not listed.
- **Misplaced components:** a registered component that is EntityLevel or not Serializable but appears under `"Components"` (a hand edit putting `"Name"` there) is the structural defect `SCENE_MISPLACED_COMPONENT`: a located error in Strict mode, dropped with its JSON in `LoadRepair::Removed` in Repair mode, never added to an entity (adding a Required component twice would assert on external input). Its fixture is `Scenes/Invalid/MisplacedComponent.scene`.
- **Invalid component values:** a known component whose value the registry rejects fails a Strict load (located Validation). A Repair load drops it, or resets it to its defaults when it is Required, and reports `SCENE_INVALID_COMPONENT` as a warning plus a `LoadRepair` holding the rejected JSON, so `scene.open {repair: true}` has defined behaviour.

### 15. Structural pre-validation details

- **Two prefab-link fixtures:** §6 lists two prefab-link defects ("InstanceRoot is missing or not an instance root", "a member whose root lacks PrefabInstanceComponent"). Both have a fixture: `PrefabLinkMissingRoot` and `PrefabLinkRootNotInstance`. `MisplacedComponent` (decision 14) is a ninth defect fixture beyond §6's list.
- **Prefab documents:** they get `PREFAB_INVALID_ROOT` ("Root" missing, naming no entity, or not the only root), which is not repairable.
- **Fresh IDs in repairs:** fresh IDs come from `LoadOptions::RepairIdGenerator` (required in Repair mode), so repairs are deterministic for a given generator state.
- **Duplicate IDs:** children that name a duplicated ID stay with its first holder.
- **Stable codes:** the diagnostic codes are constants in `Scene/LoadReport.h`.
- **Validator entry point:** the validator is a separate unit, `StructuralValidator::Validate`, which the scene and prefab loaders share.

### 16. Euler angles

`Transform.EulerAngles` (and later `Quat.FromEuler` and the inspector) use degrees, with rotation order Z, then X, then Y applied to a vector, i.e. `q = qY * qX * qZ`. The X angle is returned in [-90, 90]. The conversion goes through DetMath (§4.12).

### 17. Change tracking and override recording

`ChangeTracker` records, per touched entity, the canonical entity JSON before its first change, its parent and sibling index, and the names of the components touched. That is what `SceneEditCommand` needs (§12.3). §5.5's field-level overrides are derived by diffing instance members against the prefab (`PrefabInstantiator::ComputeOverrides`/`RefreshOverrides`), not tracked per field. Edits can arrive through typed `Patch` lambdas that do not say which fields changed, and a diff is deterministic and stays correct after undo.

- **Sibling indices:** `SiblingIndexBefore` is relative to the parent's child list as it was when the edit began. The scene records each parent's child order (`ChangeTracker::RecordChildOrder`) before the first hierarchy change under it, and `End()` derives the indices from those lists. An index taken at each entity's first touch would depend on the order in which one edit destroyed or moved siblings (deleting a multi-selection, grouping), and `End()` sorts by UUID, so undo could not restore the order. Undo takes the touched entities out of their lists and re-inserts each parent's restored children in ascending `SiblingIndexBefore`; the untouched siblings kept their relative order, so every child lands at its original index.

### 18. Prefab overrides and instance IDs

- **Override layout:** `PrefabOverride` has a `Kind` (`Field`, `AddComponent`, `RemoveComponent`, `EntityKey`) next to the §6.2 keys. The file key order is `PrefabEntityID, Kind, Component, Field, Value` (the §6.2 example is abridged). Overrides are kept sorted by (PrefabEntityID, Component, Kind, Field).
- **Entity-key overrides:** §1.2 tells users to deactivate members instead of deleting them, so an instance must be able to keep a member inactive (or renamed, or retagged) across updates. `EntityKey` overrides have an empty Component, Field `Name`, `Active` or `Tags`, and a Value resolved against the registered struct `PrefabEntityKeys`. The root's Name stays implicit; its Active and Tags are recorded like any member's.
- **Instance space:** overrides are computed after remapping the prefab entity's internal references (EntityRef fields and Entity-kind script field values) through `DeriveInstanceID`, so a fresh instance has no overrides and an internal reference is not a spurious override. Override values and `ApplyOverrides` map instance IDs back to prefab-local IDs through the member table, so neither overrides nor the prefab asset store derived IDs.
- **Shared options:** every operation takes `PrefabOptions {Schemas}`, explicitly and without a default, because remapping, diffing and mapping back Entity-kind script fields all need the script field schemas; later additions go into the struct instead of changing signatures.
- **Instance IDs:** the instance root gets `RootID` itself, and every other member gets `Hash64(RootID, PrefabEntityID)`.
- **Collisions:** a derived ID already used in the scene is InvalidState. It is not re-hashed, because member IDs must stay a pure function of the root.

### 19. Test conventions found while writing the skeletons

- **The registry suite** of §5.4 lives in `Tests/Source/Engine/Reflection/RegistrySuiteTests.cpp`, one file across every type. The case names start with `TypeRegistry:`.
- **glm values in CHECKs:** a translation unit that `CHECK`s a glm value without seeing `Support/GlmApprox.h` instantiates the primary `StringMaker`, an ODR violation against the specialization whose winner depends on the linker and link order (here it made an unrelated GlmApprox test print `{?}`). `Tests/Source/TestsPCH.h`, which every Tests file includes first, therefore includes `Support/GlmApprox.h`; the contract task made that one-line change to the M1 file, and no stream touches it in M3.
- **Cross-unit test files:** two suites span units by design: `RegistrySuiteTests.cpp` (every registered type) and `AuthoredFilesTests.cpp` (every file under `Tests/Data`). `FieldTypeTests.cpp` also covers `RunModes`, which `FieldType.h` defines.
- **Shared support files:**
  - `Test::SceneTestFixture` and `CreateBuiltinRegistry`: a frozen registry, a seeded deterministic generator and an empty scene.
  - `Test::RegisterReflectionTestTypes`: one field of every FieldType, independent of the built-in components.
  - `Test::FixtureSchemaSource`.
  - Each support unit has its own test file (`FixtureSchemaSourceTests.cpp`, `SceneTestFixtureTests.cpp`, `ReflectionTestTypesTests.cpp`), as M1's support units do. `TypeInfoTests.cpp` covers the finished `TypeInfo.h` code (`TypeKeyOf`, `DeduceFieldType`, `MakeTypeOps`) with tests that run now; only its `Value` read and write case waits for stream A.

### 20. File ownership for the parallel streams

| Stream | Owns |
|---|---|
| A Reflection | `Engine/Source/Engine/Reflection/**`; `Tests/Source/Engine/Reflection/**` (including `RegistrySuiteTests.cpp` and `TypeInfoTests.cpp`); `Tests/Source/Support/{FixtureSchemaSource,ReflectionTestTypes}.{h,cpp}` and their `*Tests.cpp` |
| B Scene core | `Scene/{Scene,Entity,ChangeTracker,TransformSystem,ComponentAccess}.{h,cpp}`, `Scene/{ComponentHostOps,ComponentRegistration}.h`, `Scene/Components/RuntimeComponents.h`; tests `Tests/Source/Engine/Scene/{Scene,Entity,ChangeTracker,TransformSystem,ComponentAccess,ComponentRegistration}Tests.cpp`; `Tests/Source/Support/SceneTestFixture.{h,cpp}` and `SceneTestFixtureTests.cpp` |
| C Serialization and project | `Scene/{SceneSerializer,StructuralValidator,Migrations}.{h,cpp}`, `Scene/LoadReport.h`, `Scene/{SceneStateHash,EntitySnapshot}.cpp` (they implement `Scene::ComputeStateHash` and `Scene::CaptureEntitySnapshot`), `Scene/Components/UnknownComponentsComponent.h`, `Engine/Source/Engine/Project/**`; tests `Tests/Source/Engine/Scene/{SceneSerializer,StructuralValidator,Migrations}Tests.cpp`, `Tests/Source/Engine/Project/**`, `Tests/Source/Engine/Core/Json/AuthoredFilesTests.cpp`; `Tests/Source/Support/TestData.{h,cpp}` and `TestDataTests.cpp`; fixtures `Tests/Data/{Scenes,Formats,Project}/**` |
| D Components | every other `Scene/Components/*.h`, `Scene/Components/BuiltinComponents.{h,cpp}`, `Scene/Registration/*.cpp`, `Engine/Source/Engine/Asset/{AssetHandle,AssetType,TypedAssetHandle}.h`; tests `Tests/Source/Engine/Scene/Components/**`, `Tests/Source/Engine/Scene/Registration/**`, `Tests/Source/Engine/Asset/**` |
| E Prefabs (after B and C land) | `Scene/{Prefab,PrefabInstantiator}.{h,cpp}`; tests `Tests/Source/Engine/Scene/{Prefab,PrefabInstantiator}Tests.cpp`; `Tests/Data/Prefabs/**` |

- **Frozen headers:** every M3 public header is frozen, and changing one needs the contract owner's review. The Roadmap names `TypeRegistry.h`, `Scene.h` and `Entity.h`, but the other streams program against all of them.
- **Dependencies between streams:** B, C and D start in parallel, E after B and C. B's `Scene.cpp` calls `Scene::CaptureEntitySnapshot`, which C implements; B's change-tracker tests therefore check what the tracker records (kinds, places, component names) and C's `SceneSerializerTests.cpp` checks the snapshot contents, so neither stream's tests wait for the other's code. Stream A's code underlies every other stream's tests, which run once A lands.
- **Shared integration files:**
  - Stream C owns the premake files, `Dependencies.lua`, `Scripts/ModuleRules.json` and `.github/workflows/ci.yml`. M3 needs no change to any of them beyond the Project module that this contract added to `ModuleRules.json`: the projects glob their sources, and EnTT and `ENGINE_REPO_ROOT` are already configured.
  - Stream D owns `BuiltinComponents.h`.
  - Nobody touches `Tests/Source/TestsPCH.h` in M3 after the contract's one-line change (decision 19).
  - Nobody touches `Docs/Reference/*` in M3: it is not generated before M14.

## Requested amendments (docs owner)

- **Architecture §3:**
  - Add the `Project` module (layer 2: Core, Reflection, Platform) to the table (decision 4).
  - Reflection names `Entity` and `ComponentHostOps` only as opaque declarations (decision 1).
- **Architecture §4.7, §5.3, §7.1:** asset-reference fields are `TypedAssetHandle<AssetType>`; `AssetRef<T>` remains the loaded asset (decision 2).
- **Architecture §5.1:**
  - `SetParent` returns `Status` (and rejects unrepresentable keepWorld results); the `CreateEntity` overloads; `ResolveEntityPath`, `GetEntityPath`, `FlushPendingDestroys`, `Clear` and the interpolation Alpha; the state hash definition (decision 10).
  - `ConstEntity` and the const overloads; entity handles are passed by value (decision 10).
  - Entity paths index same-named siblings from zero and escape `\`, `/`, `[` and `]` in names (decision 10).
  - Runtime-only components neither count in the revision nor reach the change tracker (decision 7).
- **Architecture §5.3:**
  - `DisabledTag` is the "Active" key, not a reflected component.
  - ID and Relationship are EntityLevel; every entity has Tags (decision 7).
  - The runtime components M3 defines (decision 8).
- **Architecture §5.4:**
  - `AssetFilter` is the asset type name (decision 3); `RegisterComponent<T>` (decision 1).
  - `TypeInfo`, `Value`, `VariantValue`, `ResolveContext` (with OwnerType and OwnerJson) and `IFieldSchemaSource`; free-form Variant fields and `TestSuiteSettings.Parameters` as a fourth Variant user; read-time Variant problems are warnings (decisions 5 and 6).
  - Generate hooks for every type with a Validate rule, and the merge-patch rules for null members and Variant values (decision 5).
  - Hidden components are written only by the engine (decision 9).
- **Architecture §5.5, §6.2:** the `Kind` key of prefab overrides, including entity-key overrides; instance IDs and collisions; overrides derived by diff in instance space; `PrefabOptions` (decisions 17 and 18).
- **Architecture §6:**
  - Placement of unknown components and the meaning of `ComponentVersions` (decision 14).
  - Version 0 (decision 12).
  - The prefab structural code and the repair generator (decision 15).
  - Misplaced components and invalid component values in Strict and Repair loading (decision 14).
- **Architecture §6.1, §11.10:** the settings defaults, validation rules and sentinels (decision 11).
- **Architecture §5.2 or §11.5:** the Euler convention (decision 16).
- **Architecture §12.3:** undo restores sibling order from `SiblingIndexBefore`, which is relative to the child lists at the start of the edit (decision 17).
- **CodeStyle §14:** one test file per unit, except the cross-unit suites `RegistrySuiteTests.cpp` and `AuthoredFilesTests.cpp`, named after what they sweep (decision 19).
- **Roadmap M3 deliverables:**
  - the Asset headers of decision 2;
  - `Scene/{ComponentHostOps,ComponentRegistration,ComponentAccess,LoadReport,StructuralValidator}`, `Scene/Components/{RuntimeComponents,UnknownComponentsComponent,DisabledTag}.h`;
  - the test support files of decisions 13 and 19.
- **Roadmap M3 acceptance:**
  - The re-save test covers `Tests/Data/Scenes` without `Invalid/`.
  - The directory-wide JSON check (decision 13).

## Integration decisions (M3 integration task)

The integration task merged streams A to D, implemented stream E (prefabs) and acted as the contract owner for the frozen headers. Decisions 21 to 31 settle what the streams left open or decided differently from each other; the docs owner applies the amendments listed after them.

### 21. Frozen-header changes accepted at integration

No public signature changed. The changes are private members, comments and additive constants:

- `Reflection/FieldInfo.h`: a private back-reference `m_Owner` to the declaring struct, which `SetValue` uses to resolve Variant values, plus `MakeOwnerResolveContext`. Both headers befriend `Detail::ReflectionAccess` (`Reflection/Private/ReflectionWalk.h`), the one unit that walks reflected types. `Reflection/RandomValueGenerator.h`: private helpers.
- `Reflection/StructInfo.h`: validators and generators are held through `Scope`, because `UniqueFunction::operator()` is non-const and `Validate`/`Generate` are const members. `const Scope<T>` hands out a mutable `T&`, so this is a const loophole, accepted over the alternatives because:
  - `StructValidator` and `StructGenerator` are frozen public aliases of M1's `UniqueFunction`, which has no const call operator;
  - a const-callable function type for them would change a frozen M3 type and add a second callable type next to M1's;
  - every rule reaches `StructInfo` through `FieldBuilder::Validate`/`Generate`, which take plain function pointers, so no stored rule has state that a const call could mutate.
- `Reflection/FieldType.h`: the additive constant `UnitQuaternionTolerance` (decision 22), so Reflection's Quat rule and `TransformSystem::SetWorldRotation`'s assert share one tolerance and one measure (the length, in double precision). It was private to Reflection, and the assert measured the squared length, which rejected quaternions the Quat rule accepts.
- `Scene/Scene.h`: private helpers and members (the creating thread, the pending-destroy list). The constructor now asserts a frozen registry, as its comment always said. `DestroyEntity` destroys in reverse canonical order: every child before its parent and later siblings before earlier ones, so R[A[A1, A2], B] destroys B, A2, A1, A, R. The earlier phrase "deepest first" described a different order.
- `Scene/Prefab.h`: a prefab holds its whole canonical document, through the private `m_Document` that replaces `m_Entities` and a private constructor. Unknown components therefore keep their `ComponentVersions` entries through load, save and instantiation (decision 34). `ToJson` of an empty prefab is InvalidArgument, the code `PrefabInstantiator` reports for the same empty prefab; it was Validation, which describes input that breaks a rule.
- `Scene/PrefabInstantiator.h`: the comments state how overrides are applied (decisions 26, 32 and 33) and give every function's errors (decision 28).
- `Scene/LoadReport.h`: two new codes. `PREFAB_NESTED_INSTANCE` (decision 25) and `PREFAB_STALE_OVERRIDE` (decision 26, emitted by `UpdateInstance` only: `Revert` applies no overrides). `Scene/StructuralValidator.h` lists the nested-instance defect. `Scene/SceneSerializer.h` states the repair of decision 37.
- New private header `Scene/Private/EntityDocument.h`, in namespace `Utils` like the other private headers (CodeStyle §5 keeps `Detail` for what templates force into headers): the shared scene/prefab document loader, the canonical prefab document builder, single-entity creation and replacement with a document's component versions (decision 34), and the helpers both files need (unknown-version collection, entity UUID reading, locating an error in its source file).

### 22. Reflection rules fixed by stream A

- **Quaternions:** every Quat value must be a unit quaternion within 1e-3 on every read and write path, virtual fields included. `Transform.WorldRotation` normalizes a value within that tolerance and rejects any other.
- **Float bounds:** range checks and the generated schema use the bound rounded to float, so a value written exactly as the bound passes (`0.0001` against `1e-4`).
- **Variant values:** a value the canonical writer cannot save (non-finite, outside float range, invalid UTF-8) is an error on reads as well as writes.
- **FromJson errors:** a single problem is reported with its own message at its pointer. Several problems become "N invalid fields in <Struct>", located at the object. Warnings go to `ReadContext::Diagnostics` even when the read fails. `FieldInfo::ValidateJson` locates issues the same way `ValidateValue` does.
- **Validator order:** type-level validators run only after the struct's field checks pass, nested structs included. A test suite with an invalid `Overrides.CallbackBudgetMs` therefore reports only that error.
- **Struct values:** a Struct `Value` replaces the whole struct, and members it omits take their defaults.
- **Lookups and schemas:** `TypeRegistry::FindStruct`, `FindStructByKey` and `GetStructs` exclude components. `JsonSchema::ForComponents` sets `additionalProperties: false`, so an unknown component fails the schema even though loaders only warn about it.

### 23. Prefab override rules

`PrefabOverride`'s type-level rule rejects only combinations that contradict each other:

- AddComponent or RemoveComponent with a Field;
- RemoveComponent with a non-null Value;
- EntityKey with a Component, or with a Field other than `Name`, `Active` or `Tags`.

A missing target is not a type-level error. That covers a zero `PrefabEntityID` and an empty or unknown component or field. Such an override is unresolved: its Value is kept with `REFLECTION_VARIANT_UNRESOLVED`, and the next update drops it with `PREFAB_STALE_OVERRIDE`, just as an override of a vanished field is dropped. This is what lets the default-constructed `PrefabOverride` pass the registry suite's default round trip, which §5.4 requires for every reflected struct. A RemoveComponent Value resolves only when its component is one an override can target.

The resolver targets the components `PrefabInstantiator` applies overrides to: registered, written under `"Components"` and not Hidden. `Prefab` and `PrefabLink` are therefore never targets. Resolving them let an override's Value hold a whole prefab instance whose overrides held another, so a 13 KB file nested the resolution about 100 levels deep and overflowed the Debug stack (decision 36).

### 24. Prefab documents load through the scene loader

`Utils::LoadEntityDocument` is `SceneSerializer::FromJson` generalized over `DocumentKind`. `Prefab::FromJson` loads the document into a scratch scene with it, so prefabs and scenes are validated by the same code. It then writes the document back canonically through `Prefab::CreateFromEntity`. A prefab's document keys are `Format, Version, Name, Root, ComponentVersions, Entities`, and a whole-document load resets its report.

### 25. Nested instances in prefab documents

A `Prefab` or `PrefabLink` component inside a prefab document is the structural defect `PREFAB_NESTED_INSTANCE`, because prefabs never contain nested instances (§5.5). Strict loading fails with a located error. Repair loading drops the component, which flattens the instance, and keeps its JSON in `LoadRepair::Removed`.

### 26. How overrides are applied

`UpdateInstance` and `Revert` build every member's JSON first. That JSON is the prefab entity in instance space. For the root it keeps the instance's own Name, Transform and Parent. The overrides are then applied to it in this order: RemoveComponent, AddComponent, Field, EntityKey.

- **Validation:** each override is validated on its own against the registry, with the same checks as a ComponentAccess write: Requires, Excludes, UniquePerScene, field metadata and type-level rules.
- **Read semantics, not strict writes:** unresolvable Variant values are kept, as `PrefabOptions::Schemas` allows (it may be null). ComponentAccess's strict writes would reject them.
- **Stale overrides:** an override that no longer matches, or whose value is no longer valid, is dropped with a `PREFAB_STALE_OVERRIDE` warning. The surviving overrides are stored, sorted.
- **Atomicity:** every check that can fail runs before the scene changes. Members are then rewritten with `SceneSerializer::ApplyEntityJson` or created with `EntityFromJson`, so an unchanged prefab changes nothing and costs no revision.

### 27. Update semantics

- Members whose prefab entity is gone are destroyed. Their user children move to the nearest surviving ancestor and keep their local transform. A vanished member's vanished member descendants are destroyed with it.
- Member children follow the prefab's sibling order and fill the places members held, so user children keep their places among them.
- Unknown components follow the prefab, data and version (decision 34), and never become overrides; they have no schema.
- A member of a prefab entity the prefab lacks cannot be expressed as an override, so `ComputeOverrides` skips it. `ApplyOverrides` writes every member back.
- `ApplyOverrides` keeps the prefab root's own Name and Transform, because the instance's are implicit overrides.

### 28. Instantiation errors

- A used `RootID` is AlreadyExists, M1's code for creating something whose ID is taken: the caller chose it.
- A used derived ID stays InvalidState (decision 18): the caller did not choose it, and the scene is inconsistent with the IDs that instantiation derives.
- An invalid `RootTransform` is Validation.
- A unique-per-scene component that the prefab holds twice, or that the scene already has, is also Validation.

All of them are reported before anything is created. The other operations report:

- InvalidArgument when the entity they are given is not an instance root (an invalid entity, or one without the Prefab component), or when the prefab is empty, for every function that takes one;
- InvalidArgument from `UpdateInstance` and `Revert` when the instance root belongs to another scene than the one passed;
- InvalidState from `ComputeOverrides`, `RefreshOverrides` and `ApplyOverrides` for an ambiguous reference (decision 32);
- Validation from `UpdateInstance` and `Revert` when a unique-per-scene component of the rebuilt instance, after its overrides, collides with one elsewhere in the scene.

### 29. Scene state the streams left implicit

- **HierarchyDisabledTag:** the scene keeps it current in every scene, edit scenes included. It changes on `SetActive`, on `DisabledTag` changes, on reparenting and on creation. It neither counts in the revision nor reaches the tracker.
- **UnknownComponentsComponent** is not registered. Like the runtime-only components of decision 7, changes to it alone do not count in the revision. Only the serializer writes it.

### 30. Registry suite coverage

The out-of-range check covers every component of every float kind: Float, Vec2, Vec3, Vec4, Color3, Color4. For each component it checks non-finite values, Min, Max and MinMagnitude. It also checks Int32 and UInt32 Min and Max, non-unit quaternions and enum values. A rejected write must leave the object unchanged.

### 31. Contract tests corrected at integration

- `"Prefab: user-added children are preserved"` now compares overrides against the prefab the instance was just updated to. Comparing against the original asset found the edit itself.
- `CoreRegistration` tests follow decisions 22 and 23.
- The `ProjectSettings` suite test follows the validator order of decision 22.

### 32. Override values never hold an ambiguous reference

Overrides and `ApplyOverrides` store member references as prefab-local IDs and references to other entities as they are (decision 18). An entity outside the instance can have the ID of a prefab entity. That is the normal case when a prefab is created from entities that stay in the scene, because `Prefab::CreateFromEntity` keeps their IDs as prefab-local IDs. Once stored, a reference to that outside entity reads as a reference to the prefab entity, and the next update points it at the instance's own member.

- `ComputeOverrides` (and so `RefreshOverrides`) and `ApplyOverrides` reject such a reference with InvalidState, located at the member and the component or field, with a hint to reference the member or unpack the instance. Nothing is stored, and the editor reports the error when the edit commits.
- References from outside into an instance, and references to outside entities whose IDs are not prefab-local IDs, are unaffected.
- **Deferred to the contract owner:** prefab-local IDs disjoint from live scene IDs would make the case impossible instead of rejected. That needs `CreateFromEntity` to assign fresh IDs, which changes its documented behaviour, and to remap internal script field references, which needs `PrefabOptions::Schemas` in its frozen signature.

### 33. Overrides of a Map of Variants record keys

A Map of Variants (`Script.Fields`, the one such field of the built-in components) holds values its keys author independently (§11.2). A Field override that recorded the whole map would freeze every script field of the member at its value from the time of the edit, so later prefab edits to other script fields would never reach the instance.

- **Recording:** the override's Value holds only the keys whose values differ from the prefab, as an RFC 7386 patch: the instance's value, or null for a key the instance removed. Keys are in sorted order, as canonical maps are written.
- **Applying:** each key the Value names replaces the prefab's value whole (a Variant's shape belongs to its schema, decision 5) or, when null, removes it. The prefab's other keys stay as the prefab has them.
- **Unchanged:** the override's key, sort order and file layout, and every other kind of field, whose override holds the whole field.

### 34. Prefab members carry the versions of unknown components

Instantiation, updates and reverts create and rewrite members with the prefab document's `ComponentVersions`, through the internal `Utils::EntityFromJson` and `Utils::ApplyEntityJson`. Before, the versions came from other entities of the scene, which in an empty scene gave version 0. The saved scene then dropped the entry, and once the component was registered its data would have been read as the current version, skipping its migrations.

- A prefab update that moves an unknown component to a new version moves the members' version with its data.
- `ApplyOverrides` writes the members' versions back. A member that records none (a scene file that lists no version) takes the prefab's, so prefab.apply never strips a version from the asset.
- The document's versions are read only when one of its entities holds an unknown component, so the common instantiation copies nothing.

### 35. Load codes and validator codes

The codes in `Scene/LoadReport.h` name what the scene loader found and repaired in one file. The project validator's codes (§13.7) report problems across the project, and some spell the same defects differently. The two lists stay separate, and `ProjectValidator` (M4) reports a load diagnostic under its §13.7 code:

| Load code | §13.7 code |
|---|---|
| `SCENE_DUPLICATE_ID` | `ENTITY_DUPLICATE_ID` |
| `SCENE_DANGLING_PARENT`, `SCENE_PARENT_CYCLE` | `SCENE_INVALID_HIERARCHY` |
| `SCENE_INVALID_COMPONENT` for a value out of range | `COMPONENT_FIELD_OUT_OF_RANGE` |
| `SCENE_INVALID_COMPONENT` for a missing Requires component | `COMPONENT_MISSING_REQUIREMENT` |
| `SCENE_INVALID_COMPONENT` for an Excludes conflict | `COMPONENT_CONFLICT` |
| `REFLECTION_VARIANT_UNRESOLVED` and `REFLECTION_VARIANT_MISMATCH` on `Script.Fields` | `SCRIPT_UNKNOWN_FIELD_OVERRIDE` and `SCRIPT_FIELD_TYPE_MISMATCH` |

`SCENE_DUPLICATE_UNIQUE_COMPONENT`, `SCENE_NONCANONICAL_ORDER` and `SCENE_INCONSISTENT_PREFAB_LINK` are spelled the same in both lists.

- **Why not one list:** the load codes are finer. They separate a dangling parent from a cycle, and they cover defects that §13.7 does not list: misplaced components, invalid IDs, unknown keys and components, and the prefab document codes. `SCENE_INVALID_COMPONENT` also covers values that are invalid for reasons other than range, such as the wrong JSON type or a failed type-level rule.
- **What M4 does:** the validator runs its own registry checks for the `COMPONENT_*` codes, so it does not need to split `SCENE_INVALID_COMPONENT` by cause.

### 36. Variant resolution has a depth limit

Each resolved Variant value costs a full read walk on the stack, and a resolved schema can hold Variant values of its own. Decision 23 removes the one built-in path that let a file nest them without bound. As defence in depth, the reflection walks stop at 16 nested resolutions:

- reads keep a deeper value as written, with `REFLECTION_VARIANT_MISMATCH`;
- writes and validation reject it.

Legitimate data nests two or three levels, for example an AddComponent override of `Script` whose `Fields` resolve again.

### 37. Repair unpacks the members of a root whose Prefab component it drops

Structural validation checks that a member's instance root has the `"Prefab"` key, but the component's value is read later. When Repair drops an invalid root `Prefab` component, for example after a merge left an override of an unknown kind, the members' links would point at an entity that is no longer an instance root. The next strict load, the play copy included, would then reject the repaired scene.

- After reading every entity, a Repair load unpacks each member whose `InstanceRoot` has no readable `Prefab` component, the root's own link included.
- Each unpacked member is reported as `SCENE_INCONSISTENT_PREFAB_LINK`: a repair whose Removed holds the link, plus a warning. These follow the component repairs in document order.
- A Strict load never reaches this step with such a root, because the invalid component has already failed it.

## Requested amendments from integration (docs owner)

- **Architecture §5.4:** decisions 22, 23 and 30.
- **Architecture §5.1–5.3:** decisions 21 (DestroyEntity order, frozen registry) and 29.
- **Architecture §5.5:** decisions 26 to 28, 32 (ambiguous references) and 33 (Map of Variants overrides record keys).
- **Architecture §5.4:** decision 36 (Variant nesting limit) and the Hidden-component rule of decision 23.
- **Architecture §6:** decisions 25, 34 and 37, and these loading rules from stream C:
  - Every document and entity key is optional with a default, except ID. For documents: Name "Untitled", Seed 0, ComponentVersions {}, Entities []. For entities: Name "Entity", Parent null, Active true, Tags [], Components {}.
  - A Parent of `0000000000000000` means root.
  - An empty or repeated tag is a Validation error in both modes.
  - A malformed Parent, an entity that is not an object, or Components that is not an object is a Validation error that repair cannot fix.
  - A known component with no `ComponentVersions` entry is not migrated.
  - Requires and Excludes are enforced on load with `SCENE_INVALID_COMPONENT`; repair keeps the earlier component in registry order.
  - Each structural repair is also recorded as a warning diagnostic.
  - A cycle is cut at its first member in file order (clarifies "first back-edge in file order").
- **Architecture §6.1 / §11.10:** the settings rules stream C added beyond decision 11:
  - test-suite `Parameters` is null or an object;
  - Clock deltas are > 0;
  - `Overrides.CallbackBudgetMs` is 0 or at least 10;
  - input action names are non-empty;
  - physics layers are non-empty and unique.
- **Decision 9 table (§5.3):**
  - Validate rules stream D added: Camera `NearClip < FarClip`; AudioSource `MinDistance <= MaxDistance`; tags non-empty and unique (Tags and `PrefabEntityKeys`).
  - Bounds stream D chose:
    - Camera: VerticalFov 1–179°; NearClip, FarClip and OrthographicSize ≥ 0.001 m.
    - Lights: spot cone half-angles 0–89°; LightAngle 0–90°; CascadeSplitLambda 0–1.
    - PostProcess: ExposureEV −16 to 16; BloomIntensity 0–1; SsaoRadius ≥ 0.01 m.
    - Text: colours 0–1; Size ≥ 1 px; Anchor and Pivot 0–1.
    - Every colour field is limited to 0–1: the light colours, Camera.ClearColor, Environment.FallbackColor and the Text colours. Colours are linear (§5.3), and HDR strength comes from each component's Intensity.
    - RigidBody: friction, damping and maximum velocities ≥ 0; Restitution 0–1.
    - CharacterController: Height and Radius ≥ 0.001 m; MaxSlopeAngle 0–90°.
    - AudioSource: MinDistance, MaxDistance and Pitch ≥ 0.01; Volume, Rolloff and DopplerFactor ≥ 0.
  - "Dynamic mass > 0" holds through Mass ≥ 0.001 kg.
- **Architecture §13.7:** load diagnostics reach `project.validate` under the validator codes of decision 35.
