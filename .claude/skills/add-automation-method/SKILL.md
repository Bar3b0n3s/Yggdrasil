---
name: add-automation-method
description: Use this skill whenever you add or change an editor automation method (a JSON-RPC method of the AutomationServer, also exposed as an MCP tool when flagged), or change its params, result, flags or behaviour. It lists the files to touch, the param-struct conventions every method follows, the tests to add (C++ in-process round trip and Python), the generated catalogue to refresh and the coverage the method needs.
---

# Add an automation method

Automation is how agents build games (Architecture §13). Every editor-visible capability lands with its method and a Python test in the same milestone (Roadmap rule 5), and every registered method must be called by `Tests/Automation` (gate 5, §15.6). The method registry is the single source of truth for dispatch, `rpc.discover`, the MCP catalogue and coverage, so a method is added in one place and everything else follows from it.

Read first: Architecture §13.2-§13.5 and §13.7, `Engine/Source/Engine/Automation/Protocol/MethodRegistry.h` (the param-struct conventions at the top of the file are binding) and `Docs/Decisions/0008-m4-decisions.md`.

## 1. Decide the method's shape

- **Name.** `domain.verb`: a lowercase domain, a camelCase verb (`entity.create`, `project.getSettings`). The MCP tool name is derived (`entity_create`, `project_get_settings`).
- **Metadata** (`MethodSpecification`):
  - `Description`: one or two sentences, mandatory (gate 7).
  - `RequiredParams`: the camelCase members a call cannot do without.
  - `ExposeAsTool`: only for methods agents call routinely (§13.8 lists them); others stay reachable through `engine_call`.
  - `Mutates`: the method changes project files or the open scene's content. Read-only editors refuse these unless the call is a dry run. Every method accepts `ifRevision`, flagged or not. Methods that only change what the editor shows (`scene.open`, `edit.select`) are not flagged.
  - `SupportsDryRun`: the method can run in the dry-run sandbox (§13.4). File writes then go to the overlay and scene changes to a serializer copy, so most mutations support it for free. Pending methods never do.
  - `AvailableInLauncher`: only `session.*`, `rpc.discover`, `docs.get`, `project.create` and `project.open` (§12.1).
  - `AllowedInBatch`: the method may be an op of `edit.batch`. Set it only for pure reads and for methods whose every effect goes through `EditorContext::Execute` (entity edits, settings, validator fixes), so the batch's transaction can roll everything back. Never for pending methods, `edit.batch` itself, or methods that replace the open scene, write files outside a command or end the session.
  - `AvailableInRuntime`: the method is in the Runtime subset of §13.5 (M7+).
  - `TestHook`: `debug.*` methods for the Python suite only; never tools, excluded from coverage, and not `AvailableInLauncher`.
  - `TimeoutSeconds`: how long the bridge waits (default 60).
  - `Examples`: at least one; `MethodRegistry::Freeze` checks that each example's params prepare.
- **Immediate or pending.** A method that needs more than one frame returns a `PendingOperation` (`AddPending`), polled once per frame; it must release what it holds in `Cancel` (the client may disconnect).

## 2. Write the param and result structs

In the domain's header, `Editor/Source/EditorCore/Automation/<Domain>Methods.h` (a new domain gets a new header and `.cpp`):

```cpp
// entity.rename {entity, name}: renames one entity as one undo step.
struct EntityRenameParams
{
	std::string Entity{}; // an EntityRef: 16 hex digits, a unique prefix of 6+, or a path
	std::string Name{};
	SceneTarget Target = SceneTarget::Edit;
};

struct EntityRenameResult
{
	EntitySummary Entity{};
	uint32_t UndoIndex = 0;
};
```

The twelve conventions at the top of `MethodRegistry.h` are binding; in short, numbered as there:

1. **Names.** C++ and registry names `<Domain><Verb>Params` and `<Domain><Verb>Result` (`Method` before the suffix when an engine type already has the name, as `LogReadMethodResult`).
2. **Keys.** The registered field name is the JSON key: camelCase, normally the member name with its first letter lower-cased. Embedded component, settings and asset data keeps its PascalCase.
3. **Presence.** Every member is optional on read; required ones go into `RequiredParams`. Where absence means "leave unchanged", the handler asks `context.HasParam("name")`.
4. **Reserved members.** Never declare `dryRun`, `ifRevision` or `_meta`: the registry takes them out first.
5. **Strictness.** Unknown members are InvalidParams with "did you mean" hints; floats are finite and within their `FieldMeta` range.
6. **Enums.** Registered enums; the registry accepts any ASCII case and handlers see the canonical name.
7. **Entity references** are `std::string`, resolved with `EditorMethodContext::ResolveEntity`.
8. **Paths** are `std::string`, resolved with `EditorMethodContext::ResolveProjectPath`, so every method confines them to `project://` the same way (§13.2).
9. **Lists** take `limit` (1 to 1000, default 100) and `cursor` (an opaque string, `""` for the start), and return `nextCursor` (`""` when there is nothing more). Log and event cursors are decimal sequence numbers and also accept `"end"`.
10. **Component maps** are `std::map<std::string, VariantValue>` registered with `VariantField(..., &ResolveComponentValue)`. A value may also set writable virtual fields (`Transform.EulerAngles`); the registry validates them, and the handler applies them with `ComponentAccess::SetFieldValue` after the stored fields (as `entity.create` does).
11. **Polymorphic members** (`fix: true | [...]`) are free-form `VariantValue` members the handler validates.
12. **Results** carry canonical ids plus readable names and paths; edit-scene mutations report `undoIndex` as `uint32_t` (`ToAutomationCounter` saturates, as for revisions, which are `EditorContext::GetRevision`). `_meta` and `dryRun` are added by the Dispatcher.

Register the structs in the domain's `Register<Domain>MethodTypes(TypeRegistry&)`, with mandatory descriptions and `FieldMeta` ranges (`{ .Min = 1.0, .Max = 1000.0 }` for a limit). Shared structs go into `AutomationTypes.h`. A struct without fields still needs registering; discard the builder explicitly.

## 3. Write the handler

In `Editor/Source/EditorCore/Automation/<Domain>Methods.cpp`, inside `namespace Automation`:

```cpp
Result<EntityRenameResult> EntityRename(EditorMethodContext& context, const EntityRenameParams& params)
{
	ENGINE_TRY_ASSIGN(Scene* scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), true));
	ENGINE_TRY_ASSIGN(Entity entity, context.ResolveEntity(*scene, params.Entity, "/entity"));
	SceneEdit edit(context.GetEditor(), std::format("Rename '{}'", entity.GetName()));
	entity.SetName(params.Name);
	ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());
	return EntityRenameResult{ .Entity = context.MakeEntitySummary(entity), .UndoIndex = ToAutomationCounter(undoIndex) };
}
```

- **Mutations** go through `EditorContext::Execute`, normally via `SceneEdit` (one `SceneEditCommand` per call) or a `ProjectSettingsCommand`. An early return rolls the edit back, so a failed call changes nothing. Undo labels are plain ("Rename 'Board'"); the history adds `[agent] `. A new `Command` type that changes the scene overrides `Command::ReplayOnSceneCopy`, which `scene.diff {against: "revision"}` uses to rebuild earlier revisions (ADR 0008 decision 29).
- **Files** are written only through `EditorContext::WriteProjectFile`, which handles read-only editors, dry runs and provenance.
- **Large results** need no special handling: the Dispatcher offloads any result over 48 KB to a file and answers `{path, truncated, summary}` (§13.4, ADR 0008 decision 22). Offer filters or paging (`limit`/`cursor`) so agents rarely hit it.
- **Errors** are `Result` values with the right `ErrorCode` (InvalidArgument for bad params, NotFound, InvalidState, Validation...), located and with a hint where possible (§13.3). Never assert on request data. Use `context.SetErrorData` for structured extras such as `failedOp`.
- **Events** go through `EditorContext::AppendEvent`, which dry runs suppress.

## 4. Register the method

In the domain's `Register<Domain>Methods(MethodRegistry&)`:

```cpp
methods.Add<EditorMethodContext, EntityRenameParams, EntityRenameResult>(
	{ .Name = "entity.rename", .Description = "Renames an entity as one undo step.", .RequiredParams = { "entity", "name" },
		.ExposeAsTool = true, .Mutates = true, .SupportsDryRun = true, .AllowedInBatch = true,
		.Examples = { MethodExample{ .Description = "Rename the board.", .Params = Json{ { "entity", "/Board" }, { "name", "Grid" } } } } },
	&Automation::EntityRename);
```

A new domain is also added to `RegisterEditorMethodTypes` and `RegisterEditorMethods` in `Editor/Source/EditorCore/Automation/RegisterMethods.cpp`. That file is a shared integration file with one owner per milestone (Roadmap rule 4): send the change to its owner if you are not.

## 5. Test it

- **C++ in-process round trip** (§15.2) in `Tests/Source/EditorCore/Automation/<Domain>MethodsTests.cpp`, through `Test::AutomationFixture` (`Support/AutomationTestClient.h`). Cover success, every documented error, a dry run when supported, and undo for mutations. Add the method to the expected list in `RegisterMethodsTests.cpp`.
- **Python test** in `Tests/Automation/test_*.py` through the harness (`Tests/Automation/harness.py`: `open_editor_with_scene()` gives a client of a headless editor on a fresh project; `client.call(method, params)` returns the result or raises `engine_client.EngineError`, checked with `assert_engine_error`; `engine_client.load_offloaded(result)` reads an offloaded result's file). The suite must call every registered method (gate 5): every `call` is counted, and `python Scripts/Test.py --suite automation` fails naming each registered method no test called (`UNCOVERED METHOD`). Run one test with `--filter <test name>` (the coverage gate needs the whole suite). Python tests never skip: an unbuilt editor or a missing virtual environment (`python Scripts/Setup.py`) is a failure.
- **FeatureTest coverage** from M14 on (Roadmap rule 5): every new method, enum value or field gets its FeatureTest coverage in every run mode it supports.

## 6. Refresh generated files

- `Tools/MCP/catalog.json` is the output of `Editor --headless --renderer none --dump-reference <dir>` (`<dir>/catalog.json`). Regenerate and commit it whenever a tool's name, description, flags or params change; a test compares it with the editor's output. From M14 `python Scripts/GenerateDocs.py` does this, and also writes `Docs/Reference/Automation.md`.
- Update `Docs/Architecture.md` §13.5 (through its owner) when the method list changes, and the MCP tool list of §13.8 for a new tool.

## 7. Before committing

Run the `build-and-test` skill's checks and the `commit-review` skill. The review checks the method's flags against this skill, the conventions, the tests in both languages and the refreshed catalogue.
