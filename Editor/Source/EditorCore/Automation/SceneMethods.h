#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// scene.* (Architecture §13.5), the M4 subset: new, open, save, tree, query, get, diff. scene.raycast arrives with M9.
// Paths are project-relative or project:// and end with ".scene". Conventions as in MethodRegistry.h.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry enum "SceneTemplate". Basic3D (camera, sun, environment, post-process, ground) arrives with M10.
	enum class SceneTemplate : uint8_t
	{
		Empty
	};

	// Registry struct "SceneSummary": the open scene after the call.
	struct SceneSummary
	{
		std::string Path{}; // project-relative; empty for a scene never saved
		std::string Name{};
		uint32_t Revision = 0;
		bool Dirty = false;
		uint32_t EntityCount = 0;
	};

	// Registry struct "SceneLoadIssue": a load diagnostic (LoadDiagnostic), also logged at Warn or Error so it reaches _meta.
	struct SceneLoadIssue
	{
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning; // registry enum "DiagnosticSeverity"
		std::string Code{};                                        // the load code (Scene/LoadReport.h)
		std::string Message{};
		std::string Pointer{}; // JSON pointer within the document
		std::string Entity{};  // 16 hex digits, as written in the file; empty when not about one entity
	};

	// Registry struct "SceneLoadRepair": a repair a Repair load applied (LoadRepair).
	struct SceneLoadRepair
	{
		std::string Code{};
		std::string Description{};
		std::string Pointer{};
		std::string Entity{};
		VariantValue Removed{}; // the dropped component's JSON, or null
	};

	// scene.new {path, template?, save?, discardChanges?}: creates a new scene, writes it to `path` at once (so build
	// settings naming it validate) and makes it the open scene. With a dirty open scene exactly one of save and
	// discardChanges is required, as for scene.open (ADR 0008 decision 13 adds the two flags to §13.5's list). Errors:
	// AlreadyExists when `path` exists.
	struct SceneNewParams
	{
		std::string Path{};
		SceneTemplate Template = SceneTemplate::Empty;
		bool Save = false;
		bool DiscardChanges = false;
	};

	struct SceneNewResult
	{
		SceneSummary Scene{};
		uint32_t UndoIndex = 0; // always 0: opening a scene starts a new history
	};

	// scene.open {path, save?, discardChanges?, reload?, repair?} (§13.5): with a dirty open scene exactly one of save
	// (write it first) and discardChanges is required, else InvalidState; giving both is InvalidArgument. Opening the open
	// scene's own path needs reload (it re-reads the file, adopting a SceneChangedOnDisk version, §7.5). repair loads with
	// structural repairs (§6) and reports them; the scene is then dirty until saved. Load warnings are reported and logged.
	struct SceneOpenParams
	{
		std::string Path{};
		bool Save = false;
		bool DiscardChanges = false;
		bool Reload = false;
		bool Repair = false;
	};

	struct SceneOpenResult
	{
		SceneSummary Scene{};
		bool Migrated = false; // the file was in an older format (save it, or run project.upgrade, to rewrite it)
		std::vector<SceneLoadIssue> Diagnostics{};
		std::vector<SceneLoadRepair> Repairs{};
	};

	// scene.save {path?}: writes the open scene to `path` (empty: its own path; required for a scene never saved) through
	// EditorContext::WriteProjectFile (provenance) and marks it saved. Saving to another path makes that the scene's path.
	struct SceneSaveParams
	{
		std::string Path{};
	};

	struct SceneSaveResult
	{
		SceneSummary Scene{};
		std::string File{}; // project-relative path written
	};

	// Registry enum "SceneTreeFormat". The JSON format's enumerator is JsonList and its registry name "Json": an enumerator
	// named Json would shadow the alias Engine::Json, which GCC's -Wshadow reports.
	enum class SceneTreeFormat : uint8_t
	{
		Text,    // the token-efficient outline of §13.7
		JsonList // entities as a flat list in canonical order
	};

	// scene.tree {root?, depth?, format?, target?}: the hierarchy below `root` (an EntityRef; empty: every root), at most
	// `depth` levels deep (0: unlimited). Text (§13.7): a header line "<file name>  rev <n>  [dirty  ]<count> entities", then
	// one line per entity with box-drawing indentation, its name, the first 8 hex digits of its id, its components (a short
	// form such as "Camera(Ortho 11)" or "Script(Scripts/Game.luau)" where one exists) and its local translation, and
	// "… <n> children (depth limit)" where the depth limit cuts a subtree.
	struct SceneTreeParams
	{
		std::string Root{};
		uint32_t Depth = 0;
		SceneTreeFormat Format = SceneTreeFormat::Text;
		SceneTarget Target = SceneTarget::Edit;
	};

	// Registry struct "SceneTreeEntry": one entity of the JSON tree.
	struct SceneTreeEntry
	{
		std::string Id{};
		std::string Name{};
		std::string Path{};
		std::string Parent{};                  // the parent's id; empty for a root
		uint32_t Depth = 0;                    // 0 for a root (or for `root` itself)
		bool Active = true;                    // the entity's own active flag
		std::vector<std::string> Components{}; // registry names in registry order, entity-level ones excluded
		uint32_t ChildCount = 0;
		bool ChildrenOmitted = false; // the depth limit cut its children
	};

	struct SceneTreeResult
	{
		SceneSummary Scene{};
		std::string Text{};                     // format "text"
		std::vector<SceneTreeEntry> Entities{}; // format "json"
	};

	// Registry struct "SceneQueryWhere": every given criterion must hold. name: the entity name, '*' matching any run of
	// characters and '?' one character (no wildcard: exact, case-sensitive); tag: the entity has the tag; component: the
	// entity has the component (registry name); path: an entity path whose entity and descendants match.
	struct SceneQueryWhere
	{
		std::string Name{};
		std::string Tag{};
		std::string Component{};
		std::string Path{};
	};

	// scene.query {where, select?, limit?, cursor?, target?}: matching entities in canonical order, paginated. `select` names
	// components whose JSON each result carries ("all" selects every one).
	struct SceneQueryParams
	{
		SceneQueryWhere Where{};
		std::vector<std::string> Select{};
		uint32_t Limit = 100; // 1 to 1000
		std::string Cursor{}; // nextCursor of the previous page; "" for the first
		SceneTarget Target = SceneTarget::Edit;
	};

	// Registry struct "SceneQueryEntity".
	struct SceneQueryEntity
	{
		std::string Id{};
		std::string Name{};
		std::string Path{};
		std::map<std::string, VariantValue> Components{}; // the selected components' canonical JSON, by registry name
	};

	struct SceneQueryResult
	{
		std::vector<SceneQueryEntity> Entities{};
		uint32_t Total = 0;       // matches over every page
		std::string NextCursor{}; // "" when this was the last page
	};

	// scene.get {target?}: the scene's canonical document (§6.2); offloaded when over the result bound (§13.4).
	struct SceneGetParams
	{
		SceneTarget Target = SceneTarget::Edit;
	};

	struct SceneGetResult
	{
		VariantValue Scene{};
	};

	// Registry enum "SceneDiffAgainst".
	enum class SceneDiffAgainst : uint8_t
	{
		Saved,   // the scene's file on disk
		Revision // the scene as it was at `revision`, reconstructed from the history
	};

	// scene.diff {against, revision?} (§13.5, §13.7): what changed, per entity, as RFC 6902 patches from the old to the
	// current entity JSON (JsonPatchDiff). Against "revision", `revision` must be a revision the history holds (a
	// RevisionBefore or RevisionAfter of a held entry, or the current one), else NotFound.
	struct SceneDiffParams
	{
		SceneDiffAgainst Against = SceneDiffAgainst::Saved;
		uint32_t Revision = 0;
	};

	// Registry enum "SceneEntityChangeKind".
	enum class SceneEntityChangeKind : uint8_t
	{
		Created,
		Destroyed,
		Modified
	};

	// Registry struct "SceneEntityDiff".
	struct SceneEntityDiff
	{
		std::string Id{};
		std::string Name{}; // the current name (the old one for a destroyed entity)
		SceneEntityChangeKind Change = SceneEntityChangeKind::Modified;
		std::vector<VariantValue> Patch{}; // RFC 6902 operations; for Created and Destroyed one "add" or "remove" of ""
	};

	struct SceneDiffResult
	{
		uint32_t FromRevision = 0; // 0 for "saved"
		uint32_t ToRevision = 0;
		std::vector<SceneEntityDiff> Entities{}; // sorted by id
	};

	namespace Automation {

		// scene.new. Errors: InvalidState for a dirty scene without save or discardChanges; AlreadyExists; write errors.
		[[nodiscard]] Result<SceneNewResult> SceneNew(EditorMethodContext& context, const SceneNewParams& params);
		// scene.open. Errors: InvalidState (dirty scene, or the open path without reload); InvalidArgument (save and
		// discardChanges both); NotFound; Parse, Validation, UnsupportedVersion from loading (with the scene unchanged).
		[[nodiscard]] Result<SceneOpenResult> SceneOpen(EditorMethodContext& context, const SceneOpenParams& params);
		// scene.save. Errors: InvalidState without an open scene, or without a path for a scene never saved; write errors.
		[[nodiscard]] Result<SceneSaveResult> SceneSave(EditorMethodContext& context, const SceneSaveParams& params);
		[[nodiscard]] Result<SceneTreeResult> SceneTree(EditorMethodContext& context, const SceneTreeParams& params);
		// scene.query. Errors: InvalidArgument for an unknown component in where or select, or a malformed cursor.
		[[nodiscard]] Result<SceneQueryResult> SceneQuery(EditorMethodContext& context, const SceneQueryParams& params);
		[[nodiscard]] Result<SceneGetResult> SceneGet(EditorMethodContext& context, const SceneGetParams& params);
		// scene.diff. Errors: InvalidState for "saved" when the scene was never saved; NotFound for an unknown revision.
		[[nodiscard]] Result<SceneDiffResult> SceneDiff(EditorMethodContext& context, const SceneDiffParams& params);

	}

	// Registers the enums and structs above.
	void RegisterSceneMethodTypes(TypeRegistry& registry);

	// Registers the seven methods, all tools except scene.get (§13.8 lists scene_new, scene_open, scene_save, scene_tree,
	// scene_query and scene_diff); new and save mutate (open changes only what the editor shows, so read-only editors may
	// open scenes; its save flag writes through EditorContext); tree, query, get and diff are AllowedInBatch (new, open and
	// save replace or write the scene outside a command); tree, query and get are available in the Runtime (M7; their
	// declarations move to Engine/Automation/Methods then, ADR 0008 decision 26).
	void RegisterSceneMethods(MethodRegistry& methods);

}
