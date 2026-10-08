#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Automation/Methods/SceneMethods.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The editor's scene.* methods (Architecture §13.5): new, open, save and diff. The reads the Runtime shares (tree, query,
// get, and SceneSummary, which these results carry too) are Engine/Automation/Methods/SceneMethods.h's (M7,
// Docs/Decisions/0012-m7-decisions.md decision 12). Paths are project-relative or project:// and end with ".scene".
// Conventions as in MethodRegistry.h.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry enum "SceneTemplate". Basic3D (camera, sun, environment, post-process, ground) arrives with M10.
	enum class SceneTemplate : uint8_t
	{
		Empty
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
	// From M6 the loaded scene's prefab instances are rebuilt from the current prefab versions before it opens
	// (EditorContext::UpdatePrefabInstances, §5.5), so an externally changed prefab is adopted too; the scene then opens
	// dirty when that changed it.
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
		// scene.diff. Errors: InvalidState for "saved" when the scene was never saved; NotFound for an unknown revision.
		[[nodiscard]] Result<SceneDiffResult> SceneDiff(EditorMethodContext& context, const SceneDiffParams& params);

	}

	// Registers the enums and structs above (after RegisterSceneMethodTypes, whose SceneSummary they use).
	void RegisterEditorSceneMethodTypes(TypeRegistry& registry);

	// Registers the four methods, all tools (§13.8 lists scene_new, scene_open, scene_save and scene_diff); new and save
	// mutate (open changes only what the editor shows, so read-only editors may open scenes; its save flag writes through
	// EditorContext); diff is AllowedInBatch (new, open and save replace or write the scene outside a command).
	void RegisterEditorSceneMethods(MethodRegistry& methods);

}
