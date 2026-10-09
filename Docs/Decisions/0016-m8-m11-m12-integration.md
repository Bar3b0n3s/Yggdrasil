# 0016 — Integration of M8, M11 and M12

- **Status:** accepted by the integration of the three parallel milestones (the lead, acting as contract owner of the shared files and as docs owner).
- **Date:** 2026-10-08
- **Context:** M8 (Renderer I, ADR 0013), M11 (Physics, ADR 0014) and M12 (Audio, ADR 0015) were built in parallel from the M7 commit `20dd315`, and each passed the strict gate on its own. The branch `integrate/m8-m11-m12` merges them in that order (M8, then M11, then M12). Each ADR left seams for this merge: the validator's code list and count, the play session's hooks, the shared method registration, the importer and loader registries, the built-in catalogue and `Engine.pak`, and the debug draw list that M11's collider visualization fills and M8's renderer draws. This record says how each seam was resolved and what the integration added.

## Decisions

### 1. Textual conflicts and their resolution

Every conflict kept both sides' behaviour; nothing of a milestone was dropped.

- **M11 onto M8:** `ProjectValidator.h|.cpp` and `ProjectValidatorTests.cpp` (both milestones added validation codes, a check and tests; decision 2).
- **M12 onto M8 + M11:**
  - `ProjectValidator.cpp` and its tests: the audio codes, `CheckAudio` and the listener fix join the physics and render ones (decision 2).
  - `AssetLoaderRegistry.h|.cpp`: M8's Environment loader and M12's AudioClip loader are both registered; `GetTypes` lists them in `AssetType` order (Environment before AudioClip).
  - `ImporterRegistry.cpp`: `EnvironmentImporter` then `AudioImporter` and `SoundEffectImporter`, in `RegisterBuiltinImporters` and `RegisterAssetPipelineTypes` alike, so the registration order of the settings types stays "in the order of RegisterBuiltinImporters" and M12's types follow M8's; nine importers, `Describe` starting with `Audio`.
  - `BuiltinAssets.h`: the comments now describe both the blue noise (M8) and the audio block (M12); the placeholder comment keeps M8's environment rule (no placeholder asset, the fallback colour) and M12's silent clip.
  - `RegisterSharedMethods.h|.cpp`: physics (M11) before audio (M12), types and methods alike.
  - `PlaySession.cpp`: `GetPhysics` and the audio accessors side by side (the step order is decision 3).
  - Tests: `EditorAppTests.cpp` (the bake now counts the Default font, the Generated blue noise and the ten presets: "12 baked, 0 up to date, 2 not baked", then "0 baked, 12 up to date, 2 not baked"), `RegisterMethodsTests.cpp` (`physics.bodyInfo` and `audio.stats` in the method and batch lists), `RuntimeAutomationServerTests.cpp` (both in the Runtime subset), `AssetLoaderRegistryTests.cpp`, `BuiltinAssetsTests.cpp` (procedural entries plus 14: the font, the two environments, the blue noise and the ten presets) and `ImporterRegistryTests.cpp` (nine importers and both milestones' extensions and settings types).

### 2. One validation code list in §13.7 order

`ProjectValidator::GetCodes()` lists each code once, in the order of Architecture §13.7: M4's and M6's codes up to `PREFAB_MISSING_ASSET`, M11's ten `PHYSICS_*` codes (in `PhysicsDiagnosticCodes`' order, which M11's position test checks), M12's `AUDIO_NO_LISTENER` and `AUDIO_MULTIPLE_PRIMARY_LISTENERS`, M8's `RENDER_LIGHT_LIMIT_EXCEEDED`, then the build codes: 38 codes, `28 + PhysicsDiagnosticCodes.size()` in the count test. The scene checks run cameras, audio, dangling references, assets, physics and the light limit; their order is not observable, because the diagnostics are sorted. `FixOpenScene` applies, in one undoable `SceneEdit`, the camera fixes, the audio listener fixes, the adjacent-static-body fixes and the dangling-reference fixes, which touch disjoint fields.

### 3. Collider debug draw: the adapter now, the view flag with M9

ADR 0014 decision 16 put two things after the M8 merge: the adapter from M11's records to M8's `DebugDrawList`, and an extraction call "when the request asks for colliders", through the flag M8's snapshot would gain for `RenderViewFlags::Colliders`. M8 did not add view flags: ADR 0013 decision 5 moved the pick table and `RenderViewFlags` to M9.

- **The adapter lands now.** `AppendColliderDebugDraw(std::span<const ColliderDebugShape>, DebugDrawList&)` (`Scene/ColliderDebugDraw.h`, an addition to the frozen header that decision 16 prescribed) maps Box, Sphere and Capsule records one to one (a capsule's hemisphere centres at `Position ∓ Rotation · (0, HalfHeight, 0)`) and each vertex pair of a Lines record to a `DebugLine`, with the record's colour, duration 0 and `DebugDepthMode::Tested`, in the records' order. Scene may include `Renderer/DebugDrawList.h` (the `SnapshotHeaders` rule), so no module rule changed.
- **The extraction flag waits for M9.** Adding a request member to `ExtractRenderSnapshot` now would invent the view flag ahead of the milestone that owns it, and would break extraction's documented purity (collider records of mesh colliders load meshes through the asset manager, and play-session records need the session's `PhysicsSystem`). M9's `RenderViewFlags::Colliders` (the screenshot annotation `colliders`) and M10's `viewport.setOptions {colliders}` build the records for their view with `BuildColliderDebugDraw` (the project's layer table, the session's physics in play, the host's asset manager) and append them with `AppendColliderDebugDraw`; the Roadmap's M9 entry says so.
- **Tests.** "ColliderDebugDraw: AppendColliderDebugDraw maps each record onto the debug draw list in order" checks the mapping, the capsule axis under rotation, the ignored unpaired vertex and that existing commands stay first. The GPU test "ColliderDebugDraw: an edit scene's colliders render as debug lines in their categories' colours" renders an edit scene's game view (a Static box, a Dynamic sphere, a character and a mesh collider on a black view, no meshes) through `ViewportCapture`, the path of `viewport.screenshot`, with and without the appended records, and checks that each category's encoded colour appears where its collider projects and nowhere without the records. It is a GPU test rather than a golden, so it holds on every device class without a committed image.

### 4. The play session and the contexts

- **Step order (§5.7).** Fixed step: interpolation snapshot, input, the script phases (Play only), the transform update, `PhysicsSystem::PreStep`, `Step`, `PostStep`, the destroy flush (`PhysicsSystem::FlushDestroyed` before `Scene::FlushPendingDestroys`), the transform update. Frame phase: latch, the script phases (Play only), the destroy flush (the same order), the transform update, `AudioUpdate` (Play only: `AudioSystem::Update`, then the simulation-time pulls), render extraction. Simulate skips the script and audio phases and runs physics. The M7 step-order test, M11's physics session tests and M12's `SessionAudioTests` pass on the merged session.
- **Creation:** the scene loads, transforms update, the `PhysicsSystem` builds every body in canonical order, the reference transforms are recorded, then (Play with an engine) the `AudioSystem` is created with its voices held and started, matching §5.6's "physics bodies are created …; PlayOnStart audio starts". **Destruction:** the destructor releases the `AudioSystem` and gives audio time back; the members then go in reverse order, the `PhysicsSystem` before the scene it refers to. Audio is not simulated state; the state hash appends physics only.
- **Contexts.** `ProcessContextStep::Physics` (M11) sits between `CrashHandler` and `VulkanLoader`; `EngineContextStep::Audio` (M12) follows `Graphics` and the `AudioEngine` is the context's last member. The two milestones changed different files, and both orders follow §4.1.

### 5. Shared registrations, the catalogue and the Runtime subset

`physics.bodyInfo` and `audio.stats` are shared methods registered through `RegisterSharedMethods` (physics first), read-only, batchable, in the Runtime subset, and not MCP tools; `EditorCore/Automation/RegisterMethods.cpp` needed no change. `Tools/MCP/catalog.json` regenerated with `Editor --headless --renderer none --dump-reference` is identical to the merged file (M12's asset descriptions and M8's `debugView` description; neither new method is a tool). The automation suite calls both methods (`test_physics.py`, `test_audio.py`, `test_runtime.py`), so the coverage gate holds. The `add-automation-method` skill lists both in the Runtime subset.

### 6. Built-ins and Engine.pak

The catalogue holds M8's Generated blue noise and M12's ten presets and silent clip besides M6's entries. The exporter takes every File and Generated entry from the engine cooked cache, so one `Engine.pak` carries the Default font, the blue noise, the ten presets and, with a bake, the two environments; without a GPU and a bake the environments are left out with M8's warning. The exporter tests check the presets (type `AudioClip`, loadable with `LoadCookedAudioClip`) in both of M8's cases, with and without the GPU bake, and that the procedural silent clip is not in the pak. `Exporter.h`'s description names the presets.

### 7. Documentation

The "Requested amendments" of ADRs 0013, 0014 and 0015 are applied to `Docs/Architecture.md` (§2.2, §2.3, §3, §4.1, §4.11, §5.3, §5.6, §5.7, §6.1, §6.6, §6.8, §7.1, §7.4 to §7.6, §8.2 to §8.6, §8.9, §8.10, §8.12, §8.14, §9.1 to §9.6, §10.1 to §10.4, §11.5, §13.5, §13.7, §13.9, §15.1, §15.4, §15.5, §15.8 and Appendix C) and `Docs/Roadmap.md` (M8, M9, M11, M12, M13), each amendment citing its decision. `AGENTS.md` records the no-audio-device rule for the processes tests start. `CodeStyle.md` needed no change.
