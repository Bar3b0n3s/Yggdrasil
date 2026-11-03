# Validation codes (`project.validate`, `reference/Validation`)

A reference page of the `game-debugging` skill (`skills/game-debugging`).

`project.validate {fix: true}` applies every safe fix; `fix: ["<id or CODE>"]` only the selected ones, as one undoable command. Each diagnostic's `autoFixable` says whether that one can be fixed automatically.

| Code | Severity | What it means | Fix |
|---|---|---|---|
| `SCENE_NO_PRIMARY_CAMERA` | Warning | A scene with entities has no Primary camera. | Set Primary on the camera, or add one with `entity.create {components: {Camera: {Primary: true}}}`; the fix makes a scene's only camera Primary. |
| `SCENE_MULTIPLE_PRIMARY_CAMERAS` | Error | Several cameras are Primary. | Clear Primary on all but one; the fix keeps the first in scene order. |
| `SCENE_DUPLICATE_UNIQUE_COMPONENT` | Error | A scene file holds two Environment or PostProcess components. | `scene.open {repair: true}`, then `scene.save`. |
| `SCENE_NONCANONICAL_ORDER` | Warning | A child is listed before its parent in a scene file. | `scene.open {repair: true}`, then `scene.save`. |
| `SCENE_INVALID_HIERARCHY` | Error | A Parent names no entity, or parents form a cycle. | `scene.open {repair: true}`, then `scene.save`. |
| `SCENE_INCONSISTENT_PREFAB_LINK` | Error | A prefab member's PrefabLink does not match its instance root, an OuterPrefabEntityID names no enclosing instance, or a nested instance root records overrides. | `scene.open {repair: true}` (unpacks the member, clears the OuterPrefabEntityID or drops the nested root's overrides), then `scene.save`, or re-instantiate the prefab. |
| `ENTITY_DUPLICATE_ID` | Error | Two entities of a file share an ID. | `scene.open {repair: true}`, then `scene.save`. |
| `ENTITY_DANGLING_REFERENCE` | Warning | An entity reference names no entity of its scene. | Point it at an existing entity; the fix clears it in the open scene. |
| `COMPONENT_FIELD_OUT_OF_RANGE` | Error | A component value in a file fails the registry's checks. | Correct the value through automation, or `scene.open {repair: true}` (drops the component). |
| `COMPONENT_MISSING_REQUIREMENT` | Error | A component lacks a component it requires. | Add the required component, or repair-load the scene. |
| `COMPONENT_CONFLICT` | Error | A component shares its entity with one it excludes. | Remove one of them, or repair-load the scene. |
| `ASSET_MISSING` | Error | A reference names a handle no asset has. | Restore the asset (`edit.undo` of its deletion) or assign another. |
| `ASSET_TYPE_MISMATCH` | Error | A reference names an asset of another type, or a .meta disagrees with its source. | Assign an asset of the accepted type; the fix rewrites a mismatching .meta for the importer that takes the source, keeping its handle. |
| `ASSET_IMPORT_FAILED` | Error | An asset, its .meta or a scene file cannot be imported or loaded. | Fix the source file; `asset.info` shows the failure. |
| `ASSET_ORPHAN_META` | Warning | A .meta has no source file. | Restore the source, or let the fix trash the .meta. |
| `ASSET_DUPLICATE_HANDLE` | Error | Copied .meta files share a handle. | Let the fix give each copy a fresh handle. |
| `ASSET_ORPHAN_DEPENDENCY` | Warning | A dependency .meta names a missing owner. | Restore the owner, or let the fix trash the dependency .meta. |
| `ASSET_TANGENTS_APPROXIMATED` | Warning | A glTF primitive needs tangents but has no UVs. | Export the model with UVs or tangents. |
| `ASSET_UNSUPPORTED_UV_SET` | Warning | A glTF texture uses TEXCOORD_1, which is ignored. | Bind it to TEXCOORD_0 in the model. |
| `ASSET_VERTEX_COLORS_IGNORED` | Warning | A glTF primitive has vertex colours, which are ignored. | Bake them into a texture or material. |
| `ASSET_CONTENT_SKIPPED` | Warning | A glTF holds skins, animations, cameras, lights, points or lines. | Remove them, or rebuild cameras and lights as components. |
| `PATH_CASE_MISMATCH` | Error | A .meta or path matches a file only ignoring case. | Use the exact spelling; the fix renames the .meta. |
| `PREFAB_MISSING_ASSET` | Warning | An instance names an unregistered prefab. | Restore the prefab, or `prefab.unpack` the instance. |
| `PHYSICS_NONCONVEX_DYNAMIC` | Error | A non-convex MeshCollider is on a Dynamic body. | Set Convex, or make the body Static or Kinematic. |
| `PHYSICS_MIXED_TRIGGER` | Error | A body's colliders disagree on IsTrigger. | Make all or none triggers; move triggers to a child with its own body. |
| `PHYSICS_DYNAMIC_TRIGGER` | Error | A Dynamic body has only trigger colliders. | Make it Kinematic or Static, or clear IsTrigger. |
| `PHYSICS_ALL_DOFS_LOCKED` | Error | A Dynamic body has every axis locked. | Unlock an axis, or make it Kinematic or Static. |
| `PHYSICS_INVALID_SHAPE` | Error | A collider shape cannot be created. | Fix its size or scale, or use a mesh with volume. |
| `PHYSICS_ADJACENT_STATIC_BODIES` | Warning | Touching static bodies make a rolling body bump. | The fix adds a Static RigidBody to their common ancestor (one compound). |
| `PHYSICS_UNKNOWN_LAYER` | Error | A body names an undeclared layer (it uses Default). | Declare it in `Physics.Layers`, or name a declared layer. |
| `PHYSICS_NONUNIFORM_SCALE` | Warning | A sphere or capsule under non-uniform scale. | Use a uniform scale or a box/convex collider. |
| `PHYSICS_DYNAMIC_UNDER_MOVING_PARENT` | Warning | A Dynamic body below a moving ancestor does not follow it. | Move it out from under the ancestor, or make it Kinematic. |
| `PHYSICS_LIMIT_EXCEEDED` | Warning; Error at the limit | Too many bodies for the world's limit. | Merge colliders into compound bodies or remove bodies. |
| `SCRIPT_COMPILE_ERROR` | Error | A script does not compile or fails at load time. | Fix the reported line (`script.check`). |
| `SCRIPT_TYPE_ERROR` | Error; Warning for a Luau lint warning | The strict checker reports a finding other than a syntax error (a type error, an unsupported type function, a require it cannot resolve); one of Luau's lint warnings (an unused local, two statements on one line) is a Warning. | Fix the reported range (`script.check`); the games suite fails a game on either. |
| `SCRIPT_NOT_A_BEHAVIOUR` | Error | A Script component names a Module or TestSuite. | Assign a script that returns `Script.Define`. |
| `SCRIPT_UNKNOWN_FIELD_OVERRIDE` | Error | An override names no declared field. | Remove or rename the override. |
| `SCRIPT_FIELD_TYPE_MISMATCH` | Error | An override does not match its declared type. | Correct the override's value. |
| `INPUT_UNKNOWN_ACTION` | Error | An `Input` call names an undeclared action. | Declare it in `Input.Actions`, or fix the literal. |
| `AUDIO_NO_LISTENER` | Warning | Spatial sources but no primary listener or camera. | Add a Primary AudioListener to the camera or player. |
| `AUDIO_MULTIPLE_PRIMARY_LISTENERS` | Warning | Several listeners are Primary. | Clear all but one; the fix clears the later ones. |
| `RENDER_NO_LIGHTING` | Warning | Non-emissive meshes but no light or ambient light. | Add a light, or raise the environment's intensity. |
| `RENDER_LIGHT_LIMIT_EXCEEDED` | Warning | More than 256 enabled lights. | Disable or remove lights. |
| `RENDER_SPOT_SHADOW_BUDGET` | Warning | More than 8 shadow-casting spot lights. | Turn off shadows on less important spot lights. |
| `BUILD_START_SCENE_MISSING` | Error | StartScene names no scene file. | Create it with `scene.new`, or point StartScene at a scene. |
| `BUILD_SCENE_MISSING` | Error | An Export.BuildScenes entry names no scene file. | Create the scene; the fix removes the entry. |
| `TEST_SUITE_INVALID` | Error | A Testing.Suites entry's script is not a TestSuite, or its scene is not a registered scene asset. | Assign a `Test.Suite` script; create and save the scene (so it has its `.meta`), or leave Scene empty. |
| `SCRIPT_COMPUTED_ASSET_LOAD` | Warning | With `Export.PruneUnreferenced` on, an `Assets.Load` computes its path, so a pruned export leaves out an asset only it names. | Pass the path as a string literal, reference the asset from a component or script field, or turn the setting off. |
| `PREFAB_CYCLE` | Error | A prefab nests itself, directly or through other prefabs; a prefab update that would propagate around the cycle is refused. | Remove the nested instance that closes the cycle from one of the named prefab files (cycles come only from hand edits or merges). |
| `UI_ELEMENT_WITHOUT_CANVAS` | Warning | An entity with a `UiRect` has no `UiCanvas` on an ancestor, so it is never laid out or drawn. | Move it below a canvas (`entity_reparent`), or add a `UiCanvas` to its root; `ui_tree` lists what each canvas holds. |
| `UI_IMAGE_FRAME_OUT_OF_RANGE` | Warning | A `UiImage`'s `Frame` is not an index of its sprite sheet's `Frames`, so it draws nothing. | Set `Frame` below the sheet's frame count (`asset_get_properties` lists them), or add the frame to the sheet. |
| `UI_WIDGET_PART_INVALID` | Warning | A `UiToggle`'s `Graphic`, a `UiSlider`'s `Fill` or `Handle`, or a `UiScrollView`'s `Content` names an entity that is not a UI element below the widget in its canvas, so the widget ignores it. | Name an element below the widget (an entity with a `UiRect`, no other `UiCanvas` in between), or clear the reference; `ui_tree` shows each element's parent. |
