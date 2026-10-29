#pragma once

#include "EditorCore/Automation/AssetMethods.h"
#include "EditorCore/Automation/AutomationTypes.h"
#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/Commands/Command.h"
#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the asset-related method domains share (asset.*, prefab.*, project.refreshAssets, and the inspector's asset edits):
// the asset reference and path rules of §7.1 and §13.2, the summaries results carry, the refresh every path-taking call
// makes first (§7.3), and the prefab-instance helpers of the §5.5 "Update" flows.

namespace Engine {

	class EditorAssetManager;
	class EditorContext;
	class EditorMethodContext;
	class Prefab;
	class VirtualFileSystem;

	namespace Utils {

		// {id, path, type} of `handle` as the asset manager knows it (§7.1: references expand to AssetSummary).
		[[nodiscard]] AssetSummary MakeAssetSummary(const EditorAssetManager& assets, AssetHandle handle);

		// An AssetDiagnostic as asset results report it.
		[[nodiscard]] AssetDiagnosticInfo MakeAssetDiagnosticInfo(const AssetDiagnostic& diagnostic);

		// EditorAssetManager::Refresh, its report dropped (failed reimports are recorded as diagnostics): §7.3 "every
		// automation call that takes a path refreshes first". Errors: those of Refresh.
		[[nodiscard]] Status RefreshAssets(EditorContext& editor);

		// The handle an asset reference names (§7.1): 16 hex digits, "Assets/..." with an optional "#<key>", or an engine
		// path; a project path refreshes the registry first. Errors (not located): InvalidArgument for an empty or malformed
		// reference (ParseAssetReference's message and hint); NotFound for one that names nothing, with "did you mean"
		// suggestions among the registered paths (among the built-in paths for an engine path, M17 friction F02); the
		// refresh's errors.
		[[nodiscard]] Result<AssetHandle> ResolveAssetReference(EditorContext& editor, std::string_view reference);

		// ResolveAssetReference for an asset param, its errors located at `pointer`.
		[[nodiscard]] Result<AssetHandle> ResolveAssetParam(EditorMethodContext& context, std::string_view reference, std::string_view pointer);

		// A project path param below Assets/ (EditorMethodContext::ResolveProjectPath with `extension`). `allowAssetsRoot`
		// accepts "Assets" itself (a destination folder). Errors: those of ResolveProjectPath; InvalidArgument at `pointer` for
		// a path outside Assets/.
		[[nodiscard]] Result<VfsPath> ResolveAssetsPath(const EditorMethodContext& context, std::string_view path, std::string_view pointer,
			std::string_view extension = {}, bool allowAssetsRoot = false);

		// Appends a Directory entry (Before absent, After present) for `directory` and each missing ancestor of it below the
		// project root, outermost first, so an AssetEditCommand creates them and its undo removes them.
		void AddMissingDirectoryEdits(const VirtualFileSystem& vfs, const VfsPath& directory, std::vector<AssetFileEdit>& edits);

		// AlreadyExists at `pointer` when `path` or its .meta exists (asset.create, asset.import, prefab.create).
		[[nodiscard]] Status CheckAssetPathFree(const VirtualFileSystem& vfs, const VfsPath& path, std::string_view pointer);

		// Before an editor action gives `prefab` a new version (prefab.apply, asset.setImportSettings of an instanced glTF):
		// records the overrides of every instance of it in the open scene relative to `current`, the version the instances
		// were built from (PrefabInstantiator::RefreshOverrides: overrides are derived by diffing, ADR 0006 decision 17), as
		// one SceneEdit "Record Prefab Overrides" through EditorContext::Execute (joining an open transaction), so the update
		// that follows keeps every member edit. Nothing is recorded when nothing changes. Errors: those of RefreshOverrides
		// (InvalidState for a reference it cannot store) and of the edit.
		[[nodiscard]] Status RecordPrefabOverrides(EditorContext& editor, AssetHandle prefab, const Prefab& current);

		// Whether another Prefab asset references `prefab` (its import dependencies name it): only then can a prefab nest it
		// (Docs/Decisions/0031-m20-contract.md decision 8: the prefab importer lists every prefab a document nests).
		[[nodiscard]] bool IsReferencedByPrefab(const EditorAssetManager& assets, AssetHandle prefab);

		// Whether a new version of the asset `asset` must update prefab instances or nesting prefabs (§5.5 "Update"): it is a
		// Prefab that an open scene instantiates (IsInstancedInOpenScenes; Docs/Decisions/0031-m20-contract.md decision 15) or
		// that another prefab may nest (IsReferencedByPrefab; decision 8).
		[[nodiscard]] bool UpdatesPrefabInstances(const EditorContext& editor, AssetHandle asset);

		// The prefab assets nesting `prefab` that a new version of it rewrites, in their order (EditorContext::
		// GetNestingPrefabs). Only a prefab another prefab asset references can be nested (IsReferencedByPrefab), and only one
		// whose current version `current` nests a prefab itself can be on a nesting cycle; any other rewrites no prefab, so
		// the nesting graph is not walked for it. Errors: Validation PREFAB_CYCLE when the propagation meets a nesting cycle,
		// through `prefab` or among the prefabs nesting it; NotFound for a handle that names no Prefab asset.
		[[nodiscard]] Result<std::vector<AssetHandle>> FindNestingPrefabs(const EditorContext& editor, AssetHandle prefab, const Prefab& current);

		// The command that makes the prefab writes of one undo step reach the other open scenes (Docs/Decisions/
		// 0031-m20-contract.md decision 15): the step executes one with `afterUndo` true before its first write and one with
		// `afterUndo` false after its last command, so after every write of the step, on its execution and its redo, and after
		// every restore, on its undo, `prefabs` (main assets) are imported from what their files hold then
		// (EditorAssetManager::Reimport, since a write only schedules its import) and EditorContext::PropagatePrefabUpdate
		// rebuilds the other open scenes. Both do nothing while no other scene is open or in a dry run, and never fail (a
		// failed import is logged at Error and the scenes follow the last good version). Neither changes a file or the
		// addressed scene (ChangesScene is false); UpdatePrefabVersion composes them. `recordAgainst` (the closing command's;
		// empty for the opening one) holds the versions of the step's prefabs before its first write, which the closing
		// command's first execution passes to PropagatePrefabUpdate, so the other scenes record their overrides against them
		// as the addressed scene does (the milestone review, M1); its redo and the undo pass none.
		[[nodiscard]] Scope<Command> CreatePrefabPropagationCommand(std::vector<AssetHandle> prefabs, bool afterUndo, std::map<AssetHandle, Prefab> recordAgainst);

		// What UpdatePrefabVersion did.
		struct PrefabVersionUpdate
		{
			uint64_t UndoIndex = 0;                    // EditorTransaction::Commit's: 0 inside a transaction, or when nothing changed
			size_t UpdatedInstances = 0;               // the outermost instances in the addressed scene rebuilt (prefab.apply's count)
			std::vector<AssetHandle> UpdatedPrefabs{}; // the prefabs nesting `prefab` that were rewritten, in nesting order
		};

		// §5.5 "Update" of an editor action that gives the Prefab asset `prefab` a new version (prefab.apply,
		// asset.setImportSettings and the inspector's import settings of a glTF; Docs/Decisions/0031-m20-contract.md decisions
		// 8 and 15), as one undo step labelled `label` (an EditorTransaction, joining an open one): the overrides of the
		// addressed scene's instances of `prefab` and of `nesting` recorded against their versions before the step
		// (RecordPrefabOverrides; `current` is `prefab`'s); `write`, the command that writes `prefab`'s .prefab or .meta
		// (null when nothing is written), executed and `prefab` imported at once (EditorAssetManager::Reimport); the prefabs
		// of `nesting` rewritten from its new version (EditorContext::CreateNestingPrefabUpdateCommand); and every outermost
		// instance of `prefab` and of `nesting` in the addressed scene rebuilt (CreatePrefabUpdateCommand). The step's
		// writes, their undo and their redo reach the other open scenes (CreatePrefabPropagationCommand around them), which
		// on the step's execution record their overrides against the same versions before the rebuild.
		// `nesting` is FindNestingPrefabs's, which the caller asks before anything is written, so a nesting cycle refuses the
		// action whole. Errors: those of each part; the transaction rolls back what was done then.
		[[nodiscard]] Result<PrefabVersionUpdate> UpdatePrefabVersion(EditorContext& editor, AssetHandle prefab, const Prefab& current,
			Scope<Command> write, std::span<const AssetHandle> nesting, std::string label);

		// Executes `write`, the AssetEditCommand that writes new import settings into the .meta of the main asset `asset`
		// (asset.setImportSettings and the inspector's import settings), and returns its undo index. The write schedules the
		// asset's import, which the call does not wait for, unless a new version of it updates prefab instances
		// (UpdatesPrefabInstances): then it is UpdatePrefabVersion's step labelled `label`, the prefab's version before the
		// write loaded first. Errors: those of the write; for a Prefab that updates instances, those of loading it,
		// FindNestingPrefabs's (PREFAB_CYCLE, nothing written) and UpdatePrefabVersion's.
		[[nodiscard]] Result<uint64_t> ExecuteImportSettingsWrite(EditorContext& editor, AssetHandle asset, Scope<Command> write, std::string label);

	}

}
