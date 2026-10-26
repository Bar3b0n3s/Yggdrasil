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

// The editor's scene.* methods (Architecture §13.5): new, open, save and diff, and from M20 list, activate and close. The
// reads the Runtime shares (tree, query, get, and SceneSummary, which these results carry too) are
// Engine/Automation/Methods/SceneMethods.h's (M7, Docs/Decisions/0012-m7-decisions.md decision 12). Paths are
// project-relative or project:// and end with ".scene". Conventions as in MethodRegistry.h.
//
// Open scenes (M20, E-SCENES; Docs/Decisions/0031-m20-contract.md decisions 13 to 16). The editor keeps several scenes open,
// one of them active (EditorContext's class comment). "The open scene" below is the scene a request addresses: its scene
// target ("scene", MethodSpecification::SupportsSceneTarget) when it names one, else the active scene, so a call without a
// target behaves exactly as before M20. scene.new and scene.open replace the active scene unless keepOpen asks to keep the
// open scenes and add the new one as the active scene (decision 14); scene.list, scene.activate and scene.close manage the
// set. scene.save, scene.diff, scene.close and scene.activate take the scene target; scene.new and scene.open do not (they
// name their file with `path`).

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry enum "SceneTemplate": an empty scene. A ready-to-edit 3D scene comes with a new project, through
	// project.create {template: "Basic3D"} (ProjectMethods.h, EditorCore/Project/Basic3DTemplate.h).
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
	// M20 (decision 14): keepOpen (bool, default false) keeps every open scene open and adds the new one at the end of the tab
	// order as the active scene, so save and discardChanges are then not needed (giving either is InvalidArgument);
	// InvalidState when MaxOpenScenes scenes are open. Without keepOpen the new scene replaces the active scene in its tab
	// position. `path` cannot be the path of an open scene that the call keeps open (InvalidState naming it). The result's
	// `id` (uint32) is the new scene's open-scene id.
	struct SceneNewParams
	{
		std::string Path{};
		SceneTemplate Template = SceneTemplate::Empty;
		bool Save = false;
		bool DiscardChanges = false;
		bool KeepOpen = false;
	};

	struct SceneNewResult
	{
		SceneSummary Scene{};
		uint32_t UndoIndex = 0; // always 0: opening a scene starts a new history
		uint32_t Id = 0;        // the new scene's open-scene id (M20)
	};

	// scene.open {path, save?, discardChanges?, reload?, repair?} (§13.5): with a dirty open scene exactly one of save
	// (write it first) and discardChanges is required, else InvalidState; giving both is InvalidArgument. Opening the open
	// scene's own path needs reload (it re-reads the file, adopting a SceneChangedOnDisk version, §7.5). repair loads with
	// structural repairs (§6) and reports them; the scene is then dirty until saved. Load warnings are reported and logged.
	// From M6 the loaded scene's prefab instances are rebuilt from the current prefab versions before it opens
	// (EditorContext::UpdatePrefabInstances, §5.5), so an externally changed prefab is adopted too; the scene then opens
	// dirty when that changed it.
	// M20 (decision 14): without keepOpen the loaded scene replaces the active scene in its tab position, the other open
	// scenes untouched. keepOpen (bool, default false) keeps every open scene open and adds the loaded one at the end of the
	// tab order as the active scene (save and discardChanges are then not needed, and giving either is InvalidArgument;
	// InvalidState when MaxOpenScenes scenes are open). A path that is already open, active or not, needs reload, as the
	// open scene's own path needs it before M20 (InvalidState naming it, with the hint to use scene.activate); reload
	// re-reads that open scene in its tab position, needs save or discardChanges when it is dirty (its own unsaved changes,
	// not the active scene's), and makes it active. keepOpen and reload together are InvalidArgument: one adds a tab, the
	// other re-reads one in place. The result's `id` (uint32) is the opened scene's open-scene id.
	struct SceneOpenParams
	{
		std::string Path{};
		bool Save = false;
		bool DiscardChanges = false;
		bool Reload = false;
		bool Repair = false;
		bool KeepOpen = false;
	};

	struct SceneOpenResult
	{
		SceneSummary Scene{};
		bool Migrated = false; // the file was in an older format (save it, or run project.upgrade, to rewrite it)
		std::vector<SceneLoadIssue> Diagnostics{};
		std::vector<SceneLoadRepair> Repairs{};
		uint32_t Id = 0; // the opened scene's open-scene id (M20)
	};

	// scene.save {path?}: writes the open scene to `path` (empty: its own path; required for a scene never saved) through
	// EditorContext::WriteProjectFile (provenance) and marks it saved. Saving to another path makes that the scene's path.
	// M20: the open scene is the addressed one (scene target); saving to the path of another open scene is InvalidState
	// naming it (close that scene first), so no two open scenes share a path.
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

	// --- Open scenes (M20, E-SCENES; Docs/Decisions/0031-m20-contract.md decision 14) ------------------------------------
	// Registered by RegisterEditorSceneMethodTypes (registry names as below, keys camelCase) for the three methods that
	// RegisterEditorSceneMethods adds.

	// Registry struct "OpenSceneEntry": one open scene as scene.list reports it.
	struct OpenSceneEntry
	{
		uint32_t Id = 0;            // its open-scene id (EditorContext's OpenSceneId), which the scene target accepts
		SceneSummary Scene{};       // path (empty for a scene never saved), name, revision, dirty flag, entity count
		bool Active = false;        // the active scene
		bool ChangedOnDisk = false; // its file or a prefab it instantiates changed on disk (SceneChangedOnDisk, §7.5)
	};

	// scene.list {}: the open scenes in tab order. Never fails with a project open; empty without an open scene.
	struct SceneListResult
	{
		std::vector<OpenSceneEntry> Scenes{};
	};

	// scene.activate {scene}: makes the scene the scene target names the active scene (the tab the editor shows, play mode
	// plays and requests without a target address). The target is required (its absence is InvalidParams naming "scene").
	// Allowed while playing: the session keeps playing the scene it started from. Params: NoParams, the target being the
	// reserved member.
	struct SceneActivateResult
	{
		OpenSceneEntry Scene{}; // the now active scene
	};

	// scene.close {scene?, save?, discardChanges?}: closes the addressed scene (the active one without a target), discarding
	// its history. A dirty scene needs exactly one of save (write it to its path first; InvalidState for a scene never
	// saved, which needs scene.save {path}) and discardChanges, as scene.open does; giving both is InvalidArgument. save
	// writes only a dirty scene. When the closed scene was active, the next one in tab order becomes active (the previous
	// one when it was the last). Allowed while playing, the played scene included (the session plays on).
	struct SceneCloseParams
	{
		bool Save = false;
		bool DiscardChanges = false;
	};

	struct SceneCloseResult
	{
		SceneSummary Closed{}; // the closed scene as it was when it closed (after the save, when save was given)
		std::string File{};    // the project-relative file save wrote; empty without save
		uint32_t ActiveId = 0; // the open scene now active; 0 when no scene is open
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

		// scene.list (M20). Errors: none with a project open.
		[[nodiscard]] Result<SceneListResult> SceneList(EditorMethodContext& context, const NoParams& params);
		// scene.activate (M20). Errors: InvalidParams without a scene target; those of EditorMethodContext::ResolveSceneTarget
		// and EditorContext::ActivateScene.
		[[nodiscard]] Result<SceneActivateResult> SceneActivate(EditorMethodContext& context, const NoParams& params);
		// scene.close (M20). Errors: InvalidState "no scene open", for a dirty scene without save or discardChanges, and for
		// save of a dirty scene never saved; InvalidArgument for both flags; those of ResolveSceneTarget, the save's write and
		// EditorContext::CloseScene.
		[[nodiscard]] Result<SceneCloseResult> SceneClose(EditorMethodContext& context, const SceneCloseParams& params);

	}

	// Registers the enums and structs above (after RegisterSceneMethodTypes, whose SceneSummary they use).
	void RegisterEditorSceneMethodTypes(TypeRegistry& registry);

	// Registers the seven methods, all tools (§13.8 lists scene_new, scene_open, scene_save, scene_diff, scene_list,
	// scene_activate and scene_close); new and save mutate (open changes only what the editor shows, so read-only editors may
	// open scenes; its save flag writes through EditorContext); diff is AllowedInBatch (new, open and save replace or write
	// the scene outside a command). M20 (decision 14): list, activate and close do not mutate (they change what the editor
	// holds open, and close's save writes through EditorContext, as scene.open's does), none is AllowedInBatch (activate and
	// close change the scene a batch addresses), none is in the Runtime; SupportsSceneTarget on scene.save, scene.diff,
	// scene.activate (which also RequiresSceneTarget: its target is its subject) and scene.close.
	void RegisterEditorSceneMethods(MethodRegistry& methods);

}
