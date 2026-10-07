#pragma once

#include "EditorCore/Automation/AssetMethods.h"
#include "EditorCore/Automation/AutomationTypes.h"
#include "EditorCore/Commands/AssetEditCommand.h"
#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <string_view>
#include <vector>

// What the asset-related method domains share (asset.*, prefab.*, project.refreshAssets): the asset reference and path
// rules of §7.1 and §13.2, the summaries results carry, the refresh every path-taking call makes first (§7.3), and the
// prefab-instance helpers of the §5.5 "Update" flows.

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
		// suggestions among the registered paths; the refresh's errors.
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

	}

}
