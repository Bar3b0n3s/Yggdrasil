#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <vector>

// asset.* (Architecture §13.5, eleven methods), frozen by the M6 contract. Conventions as in MethodRegistry.h: the JSON key
// is the member name with a lower-case first letter unless commented; enums are case-insensitive and echoed canonically.
// Every "asset" param is an AssetReference (§7.1: 16 hex digits, "Assets/..." with an optional "#<sub-asset key>", or an
// engine path); every result names assets as AssetSummary {id, path, type}. Every call that takes a path first refreshes
// the registry (§7.3: "every automation call that takes a path refreshes first, so a just-written file never races the
// watcher"). File changes are AssetEditCommand, AssetMoveCommand and AssetDeleteCommand executed through
// EditorContext::Execute, so each mutation is one undo step labelled "[agent] ..." and its result carries undoIndex (0 in
// a dry run or a batch). Asset edits write through (ADR 0010): there are no dirty native assets.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class PendingOperation;
	class TypeRegistry;

	// Registry struct "AssetDiagnosticInfo": an AssetDiagnostic as asset methods report it.
	struct AssetDiagnosticInfo
	{
		DiagnosticSeverity Severity = DiagnosticSeverity::Error;
		std::string Code{};
		std::string Path{}; // project-relative or engine path
		std::string Message{};
		std::string Hint{};
	};

	// asset.list {dir?, type?, recursive?, subAssets?, limit?, cursor?}: registered assets whose source lies in `dir`
	// (project-relative, default "Assets"; with recursive, also below it), of `type` (None: every type), sorted by path, a
	// glTF's sub-assets right after it ("Assets/Track.glb#mesh:0:Straight"). Dependency files are never listed. Bounded
	// (§13.4): at most `limit` (1 to 1000) entries per call; nextCursor continues.
	struct AssetListParams
	{
		std::string Dir = "Assets";
		AssetType Type = AssetType::None;
		bool Recursive = true;
		bool SubAssets = true;
		uint32_t Limit = 100;
		std::string Cursor{}; // empty: from the start; otherwise a previous nextCursor
	};

	struct AssetListResult
	{
		std::vector<AssetSummary> Assets{};
		std::string NextCursor{}; // empty when the listing is complete
		uint32_t Total = 0;       // matching assets over every page
	};

	// asset.info {asset}: metadata, dependencies, dependents and diagnostics (§13.5).
	struct AssetInfoParams
	{
		std::string Asset{};
	};

	struct AssetInfoResult
	{
		AssetSummary Asset{};
		std::string Source{};                       // the source file (project-relative; a sub-asset's is its source's); an engine path for built-ins
		std::string Importer{};                     // the .meta's importer id; empty for procedural built-ins
		uint32_t ImporterVersion = 0;               // the registered importer's current version
		VariantValue Settings{};                    // the .meta's import settings (null without settings)
		std::vector<AssetSummary> SubAssets{};      // of a main asset; empty for a sub-asset
		std::vector<AssetSummary> Dependencies{};   // what it references (AssetDependencyGraph), sorted by id
		std::vector<AssetSummary> Dependents{};     // what references it, sorted by id
		std::vector<std::string> DependencyFiles{}; // its dependency files (§6.4), project-relative, sorted
		std::vector<AssetDiagnosticInfo> Diagnostics{};
		AssetState State = AssetState::Unloaded; // registry enum "AssetState"
		uint32_t Version = 0;                    // AssetManager::GetVersion (saturating, ADR 0008 decision 8)
	};

	// asset.import {source, destDir, settings?} (§13.2 "Paths", §13.5): copies `source` into project-relative `destDir`
	// (created when missing) and registers it. `source` is a native path (absolute, read-only access) or a project path.
	// For a .gltf the dependency closure is copied too: every buffer and image URI, as the decoded relative paths of
	// GltfImporter::ListExternalUris (percent-decoded once and validated by GltfImporter's URI rule: relative, inside the
	// source's directory tree, no ':' or backslash), with its relative path preserved under destDir and a dependency meta
	// (§6.4: the copy is one batch, so the closure's images are dependencies, EditorAssetManager.h); nothing else is copied.
	// For a native source the copy refuses a source or closure file that is a symbolic link or a reparse point, or whose
	// canonical path (std::filesystem::weakly_canonical) does not lie under the source's canonical directory, so no file
	// outside that directory is ever read. `settings` is a partial object merged over the importer's defaults
	// (EditorAssetManager::MergeImportSettings). The copies and their metas are one AssetEditCommand (undo removes them),
	// then the asset is imported: a pending operation resolving when the import completed. Not dry-runnable (§13.4).
	struct AssetImportParams
	{
		std::string Source{};
		std::string DestDir{};
		VariantValue Settings{}; // an object or null
	};

	struct AssetImportResult
	{
		AssetSummary Asset{};
		std::vector<AssetSummary> SubAssets{};
		std::vector<std::string> CopiedFiles{}; // project-relative, sorted (the .meta files included)
		std::vector<AssetDiagnosticInfo> Diagnostics{};
		uint32_t UndoIndex = 0;
	};

	// asset.reimport {asset}: EditorAssetManager::ReimportAsync of a main asset, bypassing the cache; a pending operation.
	// Not undoable itself (derived data; a changed sub-asset list rewrites the .meta through the AssetWriter). When the asset
	// is a Prefab the open scene instantiates and its content changed, the operation then executes
	// EditorContext::CreatePrefabUpdateCommand (an undo step of its own, "Update Prefab Instances") before it resolves.
	struct AssetReimportParams
	{
		std::string Asset{};
	};

	struct AssetReimportResult
	{
		AssetSummary Asset{};
		std::vector<AssetSummary> SubAssets{};
		std::vector<AssetDiagnosticInfo> Diagnostics{};
	};

	// Registry enum "AssetCreateType" (§13.5 asset.create {type: Material|Scene|Prefab|SoundEffect|Folder}).
	enum class AssetCreateType : uint8_t
	{
		Material,
		Scene,
		Prefab,
		SoundEffect, // M12: a .sfx sound effect (§6.6)
		Folder
	};

	// asset.create {type, path, values?}: creates a native asset at project-relative `path` (below Assets/, with the type's
	// extension: .material, .scene, .prefab, .sfx) with its .meta, or a folder. `values` is a partial object of registry
	// PascalCase keys over the defaults: MaterialData's ("Material") for a Material, and, from M12, the sound effect's
	// ("SoundEffect", Audio/SoundSynth.h, written with SoundEffectToText) for a SoundEffect, which must then pass
	// ValidateSoundEffect (at least one layer; every violation InvalidParams located under /values, such as
	// /values/Layers/0/Notes/2); for the other types it must be absent. A Scene is an empty scene named after the file stem;
	// a Prefab holds one root entity named after the file stem. The new asset's id and type are its importer's main type
	// (a SoundEffect is an AudioClip, §7.4). One AssetEditCommand; supports dry runs (the files then go to the overlay,
	// §13.4).
	struct AssetCreateParams
	{
		AssetCreateType Type = AssetCreateType::Material;
		std::string Path{};
		VariantValue Values{};
	};

	struct AssetCreateResult
	{
		AssetSummary Asset{}; // empty id and type None for a Folder
		std::string Path{};   // project-relative
		uint32_t UndoIndex = 0;
	};

	// asset.getProperties {asset}: the properties of a native asset (§13.5: materials; sound effects from M12, the .sfx
	// document's fields after its header) as its registry JSON (PascalCase, every field).
	struct AssetGetPropertiesParams
	{
		std::string Asset{};
	};

	struct AssetGetPropertiesResult
	{
		AssetSummary Asset{};
		VariantValue Values{};
	};

	// asset.setProperties {asset, values}: an RFC 7386 merge patch of a native asset's properties (null resets a field),
	// validated through the registry and written as one AssetEditCommand; supports dry runs.
	struct AssetSetPropertiesParams
	{
		std::string Asset{};
		VariantValue Values{}; // an object
	};

	struct AssetSetPropertiesResult
	{
		AssetSummary Asset{};
		VariantValue Values{}; // after the patch, every field
		uint32_t UndoIndex = 0;
	};

	// asset.getImportSettings {asset}: the import settings of a main asset's .meta (every field of its importer's settings
	// struct).
	struct AssetGetImportSettingsParams
	{
		std::string Asset{};
	};

	struct AssetGetImportSettingsResult
	{
		AssetSummary Asset{};
		std::string Importer{};
		VariantValue Settings{};
	};

	// asset.setImportSettings {asset, settings}: an RFC 7386 merge patch of the import settings, validated against the
	// importer's settings struct, written to the .meta as one AssetEditCommand; the asset reimports (asynchronously, like
	// any write: the result does not wait for the import). Exception, §5.5 "Update": when the asset is a Prefab the open
	// scene instantiates (a glTF), the reimport runs before the method returns and the AssetEditCommand is composed with
	// EditorContext::CreatePrefabUpdateCommand into one undo step (a CompositeCommand), so the instances follow the new
	// settings (Scale, MergeMeshes) and one undo restores both.
	struct AssetSetImportSettingsParams
	{
		std::string Asset{};
		VariantValue Settings{}; // an object
	};

	struct AssetSetImportSettingsResult
	{
		AssetSummary Asset{};
		std::string Importer{};
		VariantValue Settings{}; // after the patch, every field
		uint32_t UndoIndex = 0;
	};

	// asset.move {asset, path}: AssetMoveCommand of a main asset to project-relative `path` (below Assets/); the handle and
	// every reference survive (§7.3). Refused for an asset that another asset found by path (a standalone texture a glTF
	// reuses, EditorAssetManager::GetPathDependents), whose reference would break.
	struct AssetMoveParams
	{
		std::string Asset{};
		std::string Path{};
	};

	struct AssetMoveResult
	{
		AssetSummary Asset{};                  // with its new path
		std::vector<std::string> MovedFiles{}; // the new project-relative paths, the .meta files and dependencies included
		uint32_t UndoIndex = 0;
	};

	// asset.delete {asset}: AssetDeleteCommand of a main asset (to Library/Trash/, undoable; never a permanent delete). When
	// another asset found it by path (EditorAssetManager::GetPathDependents) the delete still runs and logs a warning naming
	// those assets, whose next import fails until it is restored.
	struct AssetDeleteParams
	{
		std::string Asset{};
	};

	struct AssetDeleteResult
	{
		AssetSummary Asset{};                    // as it was
		std::string TrashDirectory{};            // project-relative, "Library/Trash/<entry>"
		std::vector<std::string> TrashedFiles{}; // the old project-relative paths, the .meta files and dependencies included
		uint32_t UndoIndex = 0;
	};

	namespace Automation {

		// asset.list. Errors: InvalidArgument for a dir outside Assets/, a limit outside 1 to 1000 or a malformed cursor;
		// NotFound for a dir that does not exist.
		[[nodiscard]] Result<AssetListResult> AssetList(EditorMethodContext& context, const AssetListParams& params);
		// asset.info. Errors: InvalidArgument for a malformed reference; NotFound for an unknown asset (with suggestions).
		[[nodiscard]] Result<AssetInfoResult> AssetInfo(EditorMethodContext& context, const AssetInfoParams& params);
		// asset.import. Errors (immediate): InvalidArgument for a destDir outside Assets/, a source without an importer, or a
		// settings value that is not an object; NotFound for a missing source; ImportFailed naming a rejected URI of the
		// closure; PermissionDenied for a symbolic link, a reparse point or a closure file outside the source's directory;
		// AlreadyExists when a destination file exists. Errors (when the operation resolves): the import's.
		[[nodiscard]] Result<Scope<PendingOperation>> AssetImport(EditorMethodContext& context, const AssetImportParams& params);
		// asset.reimport. Errors: as asset.info; InvalidArgument for a sub-asset, dependency or built-in; the import's errors
		// when the operation resolves.
		[[nodiscard]] Result<Scope<PendingOperation>> AssetReimport(EditorMethodContext& context, const AssetReimportParams& params);
		// asset.create. Errors: InvalidArgument for a path outside Assets/, a wrong extension, values given for a type that
		// takes none; Unsupported for SoundEffect before M12; AlreadyExists; Validation for invalid values (located under
		// /values).
		[[nodiscard]] Result<AssetCreateResult> AssetCreate(EditorMethodContext& context, const AssetCreateParams& params);
		// asset.getProperties. Errors: as asset.info; InvalidArgument for an asset that is not native (with the hint
		// "use asset.getImportSettings").
		[[nodiscard]] Result<AssetGetPropertiesResult> AssetGetProperties(EditorMethodContext& context, const AssetGetPropertiesParams& params);
		// asset.setProperties. Errors: as asset.getProperties; InvalidArgument for values that are not an object; Validation
		// (located under /values) for an invalid result.
		[[nodiscard]] Result<AssetSetPropertiesResult> AssetSetProperties(EditorMethodContext& context, const AssetSetPropertiesParams& params);
		// asset.getImportSettings. Errors: as asset.info; InvalidArgument for a sub-asset or built-in.
		[[nodiscard]] Result<AssetGetImportSettingsResult> AssetGetImportSettings(EditorMethodContext& context,
			const AssetGetImportSettingsParams& params);
		// asset.setImportSettings. Errors: as asset.getImportSettings; InvalidArgument for settings that are not an object or
		// an importer without settings; Validation (located under /settings); for an instanced Prefab, the import's errors
		// and those of EditorContext::CreatePrefabUpdateCommand (nothing changed then).
		[[nodiscard]] Result<AssetSetImportSettingsResult> AssetSetImportSettings(EditorMethodContext& context,
			const AssetSetImportSettingsParams& params);
		// asset.move. Errors: as asset.info; those of AssetMoveCommand::Create (InvalidState naming the assets that found it by
		// path) and Execute.
		[[nodiscard]] Result<AssetMoveResult> AssetMove(EditorMethodContext& context, const AssetMoveParams& params);
		// asset.delete. Errors: as asset.info; those of AssetDeleteCommand::Create and Execute.
		[[nodiscard]] Result<AssetDeleteResult> AssetDelete(EditorMethodContext& context, const AssetDeleteParams& params);

	}

	// Registers AssetDiagnosticInfo, the enums AssetState and AssetCreateType, and the params and result structs above.
	void RegisterAssetMethodTypes(TypeRegistry& registry);

	// Registers the eleven methods. Tools (§13.8): asset.list, asset.import, asset.create, asset.setProperties, asset.move,
	// asset.delete; the others through engine_call. Mutates: import, reimport, create, setProperties, setImportSettings, move,
	// delete. SupportsDryRun: create and setProperties (§13.4). AllowedInBatch: list, info, getProperties, getImportSettings,
	// create, setProperties, setImportSettings, move and delete (every effect goes through EditorContext::Execute, so the
	// Tetris scaffold's batch of asset.create ops is atomic, §13.11); import and reimport are pending operations and are not.
	// None is available in the launcher state; asset.info and asset.list are not in the Runtime subset.
	void RegisterAssetMethods(MethodRegistry& methods);

}
