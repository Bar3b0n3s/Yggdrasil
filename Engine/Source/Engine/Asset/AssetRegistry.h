#pragma once

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The project's asset registry (Architecture §7.3): built at project open by scanning Assets/**.meta, so the handles live
// in the sidecars and there is no central registry file. A moved pair (file + .meta) keeps its handle. The scan reports
// the §7.3 diagnostics; their fixes are planned here (pure) and applied by the editor through its write path
// (ProjectValidator::Fix runs each AssetScanFix as an AssetEditCommand or AssetMoveCommand), so every change is undoable
// and attributed.

namespace Engine {

	class VirtualFileSystem;

	// One registered .meta and its source file.
	struct AssetRecord
	{
		AssetMetadata Metadata{};
		VfsPath SourcePath{}; // "project://Assets/Models/Track.glb"
		VfsPath MetaPath{};   // GetMetaPath(SourcePath)

		bool operator==(const AssetRecord&) const = default;
	};

	// Where a handle's .meta was registered when the editor last scanned the project: the EditorAssetManager keeps these in
	// its local, gitignored record <CacheRoot>/AssetLocations.json (EditorAssetManager.h), and Scan uses them to keep a
	// duplicated handle with its original file on a cold open (the keeper rule, Scan).
	struct AssetKnownLocation
	{
		AssetHandle Handle{};
		VfsPath SourcePath{}; // "project://Assets/Models/Track.glb"

		bool operator==(const AssetKnownLocation&) const = default;
	};

	// What the scan needs to know about one importer (AssetPipeline's ImporterRegistry provides these; Asset cannot include
	// it, §3): which extensions it takes and the main type it produces, for SourcesWithoutMeta and the type/importer
	// mismatch check.
	struct AssetImporterDescription
	{
		std::string Id{};                      // "Gltf"
		AssetType MainType = AssetType::None;  // AssetType::Prefab for glTF
		std::vector<std::string> Extensions{}; // lower case with the dot: ".gltf", ".glb"; matched ASCII case-insensitively
		uint32_t Version = 0;                  // the importer's version (the ImporterVersion of a .meta a RewriteImporter fix writes)
	};

	// What a scan found besides the records.
	struct AssetScanResult
	{
		// The §7.3 diagnostics, sorted by (Path, Code, Subject): ASSET_DUPLICATE_HANDLE, ASSET_ORPHAN_META,
		// ASSET_TYPE_MISMATCH, PATH_CASE_MISMATCH, ASSET_ORPHAN_DEPENDENCY, and ASSET_IMPORT_FAILED for a .meta that cannot be
		// read (it is left alone: replacing it would change the asset's handle and break every reference).
		std::vector<AssetDiagnostic> Diagnostics{};
		// Sources an importer takes (by extension) that have no .meta, sorted: "a source without a .meta gets one" (§7.3),
		// which the editor writes (EditorAssetManager). A source matched by a .meta of another letter case is not listed
		// (it is reported as PATH_CASE_MISMATCH instead).
		std::vector<VfsPath> SourcesWithoutMeta{};
		// The number of .meta files read.
		size_t MetaCount = 0;
	};

	// A planned rename (fixes, moves, trash). Applied in order; the reverse order undoes it.
	struct AssetFileMove
	{
		VfsPath From{};
		VfsPath To{};

		bool operator==(const AssetFileMove&) const = default;
	};

	// The fix of one auto-fixable scan diagnostic.
	enum class AssetScanFixKind : uint8_t
	{
		AssignNewHandle,        // ASSET_DUPLICATE_HANDLE: rewrite the .meta with a fresh handle (sub-asset handles re-derived)
		TrashMeta,              // ASSET_ORPHAN_META, ASSET_ORPHAN_DEPENDENCY: move the .meta to the trash
		RenameMetaToSourceCase, // PATH_CASE_MISMATCH: case-only rename of the .meta to its source's spelling
		RewriteImporter         // ASSET_TYPE_MISMATCH: rewrite the .meta for the importer that takes the source, keeping its handle
	};

	struct AssetScanFix
	{
		AssetScanFixKind Kind = AssetScanFixKind::AssignNewHandle;
		VfsPath MetaPath{};
		// RenameMetaToSourceCase: the .meta's new path. AssignNewHandle: the .meta's new text (SerializeAssetMetadata with the
		// handle the caller passed). RewriteImporter: the .meta's new text, with its Handle, the Importer, Type and
		// ImporterVersion of the importer that takes the source's extension, null Settings (that importer's defaults, which
		// every import merges; the editor writes them out, EditorAssetManager::MergeImportSettings) and no SubAssets (the next
		// import lists them). TrashMeta: unused (the caller picks the trash location).
		VfsPath NewMetaPath{};
		std::string NewMetaText{};
	};

	// The registry. Main thread only (the editor's import jobs receive copies of what they need). Not thread-safe; copyable,
	// so a dry run can snapshot it (EditorAssetManager::BeginDryRun) and restore it.
	class AssetRegistry
	{
	public:
		// Where a loadable handle lives: a main asset or a sub-asset (never a dependency, which cannot be loaded).
		struct Location
		{
			const AssetRecord* Record = nullptr; // the source's record
			AssetType Type = AssetType::None;    // the main type, or the sub-asset's type
			std::string SubAssetKey{};           // empty for the main asset
		};

		AssetRegistry() = default;

		// Replaces the content with a scan of `root` (normally "project://Assets") on `vfs`: every .meta below it is read
		// (ParseAssetMetadata) and paired with its source by exact spelling; the diagnostics of AssetScanResult are computed
		// deterministically from sorted listings. A .meta that cannot be read, a duplicate and an orphan are not registered.
		//
		// The keeper rule for a handle that several .meta files carry (ASSET_DUPLICATE_HANDLE), which never depends on how
		// the copy is named (a file manager's "Wood - Copy.png", "Models - Copy/" or "Wood copy.png" sorts before the
		// original in byte order):
		//   1. Orphan .meta files (no source) never take part: they are ASSET_ORPHAN_META, so a copied sidecar such as
		//      "Wood.png - Copy.meta" can never take a handle from its original.
		//   2. The .meta at the source path this registry holds the handle at before the scan keeps it (the previous scan
		//      plus the edits since: Refresh and hot reload rescan the same registry, so a copy made while the project is
		//      open always loses).
		//   3. Otherwise the .meta at the handle's entry in `knownLocations` keeps it (a cold open: the EditorAssetManager
		//      passes its local record of the last session's locations).
		//   4. Otherwise the shortest source path (UTF-8 bytes) keeps it, and among equally long paths the first in byte
		//      order: copies are named by appending to the original's name.
		// Every other .meta of that handle is reported (AssignNewHandle fix) and not registered.
		//
		// `knownLocations` is sorted by handle, each handle once (asserted). Errors: those of VirtualFileSystem::List on
		// `root` (NotFound when it does not exist); a file that cannot be read becomes a diagnostic, never an error.
		[[nodiscard]] Result<AssetScanResult> Scan(const VirtualFileSystem& vfs, const VfsPath& root,
			std::span<const AssetImporterDescription> importers, std::span<const AssetKnownLocation> knownLocations = {});

		// The diagnostics of the last Scan, kept until the next one.
		[[nodiscard]] std::span<const AssetDiagnostic> GetScanDiagnostics() const { return m_ScanDiagnostics; }

		// --- Lookups ---------------------------------------------------------------------------------------------------

		// The record whose .meta carries `handle` (a main asset or a dependency), or nullptr.
		[[nodiscard]] const AssetRecord* Find(AssetHandle handle) const;
		// The record of the source at exactly `sourcePath`, or nullptr.
		[[nodiscard]] const AssetRecord* FindBySourcePath(const VfsPath& sourcePath) const;

		// The location of a main asset or sub-asset `handle`, or nullopt (unknown, or a dependency).
		[[nodiscard]] std::optional<Location> Locate(AssetHandle handle) const;

		// The handle a reference names (AssetReference syntax, §7.1) among the registered assets: a handle that Locate knows,
		// a project path of a registered source (its main asset), or a sub-asset path. Engine paths and unknown names give
		// nullopt (built-ins are resolved by the asset managers, BuiltinAssetCatalog). Case-sensitive (§4.10).
		[[nodiscard]] std::optional<AssetHandle> Resolve(std::string_view reference) const;

		// The readable reference of a main asset or sub-asset ("Assets/Models/Track.glb#mesh:0:Straight"); empty when Locate
		// does not know it.
		[[nodiscard]] std::string GetReferencePath(AssetHandle handle) const;

		// Every main asset and sub-asset handle (no dependencies), sorted.
		[[nodiscard]] std::vector<AssetHandle> GetHandles() const;
		// Every record, dependencies included, sorted by source path.
		[[nodiscard]] std::vector<const AssetRecord*> GetRecords() const;
		// The dependency records whose Owner is `owner`, sorted by source path.
		[[nodiscard]] std::vector<const AssetRecord*> GetDependencyRecords(AssetHandle owner) const;
		[[nodiscard]] size_t GetRecordCount() const { return m_Records.size(); }
		// Every registered handle (main assets and dependencies) with its source path, sorted by handle: what the
		// EditorAssetManager writes to <CacheRoot>/AssetLocations.json after each scan, for the next cold open's Scan.
		[[nodiscard]] std::vector<AssetKnownLocation> GetKnownLocations() const;

		// --- Edits (EditorAssetManager, after its writes) ---------------------------------------------------------------

		// Registers `record` (MetaPath must be GetMetaPath(SourcePath)). Errors: AlreadyExists when its handle, one of its
		// sub-asset handles or its source path is registered (for a new .meta that carries a registered handle, the caller
		// reports ASSET_DUPLICATE_HANDLE instead: the registered path keeps it, keeper rule 2 of Scan); InvalidArgument for
		// an inconsistent record.
		[[nodiscard]] Status Add(AssetRecord record);
		// Replaces the record with the same handle (its .meta was rewritten: new settings or sub-assets). Errors: NotFound;
		// AlreadyExists for a new sub-asset handle another record holds; InvalidArgument as Add.
		[[nodiscard]] Status Update(AssetRecord record);
		// Unregisters `handle` (a main asset's or a dependency's .meta). Errors: NotFound.
		[[nodiscard]] Status Remove(AssetHandle handle);
		// Moves the record of `handle` to `newSourcePath` (and its .meta path), keeping the handle (§7.3). Errors: NotFound;
		// AlreadyExists for a registered destination.
		[[nodiscard]] Status Rename(AssetHandle handle, const VfsPath& newSourcePath);

		// --- Plans (pure) -----------------------------------------------------------------------------------------------

		// The renames that move the main asset `handle` to `newSourcePath`: its source, its .meta, then every dependency file
		// and its dependency meta, keeping their paths relative to the source's directory, so the source's relative URIs stay
		// valid (§6.4 "moves ... together with its owner"). Errors: NotFound; InvalidArgument for a sub-asset or dependency
		// handle, a destination outside the Assets folder of the source's scheme ("project://Assets") or equal to the current
		// path; AlreadyExists when a
		// destination is a registered path.
		[[nodiscard]] Result<std::vector<AssetFileMove>> PlanMove(AssetHandle handle, const VfsPath& newSourcePath) const;

		// The renames that move the main asset `handle` into `trashDirectory` (project://Library/Trash/<entry>): the same files
		// as PlanMove, each to trashDirectory joined with its path relative to the project root (§12.3 "moves file + .meta to
		// Library/Trash/"). Errors: NotFound; InvalidArgument for a sub-asset or dependency handle.
		[[nodiscard]] Result<std::vector<AssetFileMove>> PlanTrash(AssetHandle handle, const VfsPath& trashDirectory) const;

		// The fix of an auto-fixable diagnostic of the last scan (identified by its Code, Path and Subject). `newHandle` is the
		// fresh handle for AssignNewHandle (from the editor's generator; ignored otherwise). Errors: NotFound for a diagnostic
		// the last scan did not report; InvalidArgument for one that is not auto-fixable.
		[[nodiscard]] Result<AssetScanFix> PlanFix(const AssetDiagnostic& diagnostic, AssetHandle newHandle) const;
	private:
		// The record of a main asset `handle` that a move or trash plan may take, with its error otherwise.
		[[nodiscard]] Result<const AssetRecord*> FindMovableRecord(AssetHandle handle) const;
		// Checks that `record` can be registered next to the current records, ignoring the record registered under `ignored`
		// (Update's own previous record).
		[[nodiscard]] Status CheckInsertable(const AssetRecord& record, AssetHandle ignored) const;
		void Insert(AssetRecord record);
		void Erase(AssetHandle handle);
	private:
		// What PlanFix needs to know about one auto-fixable diagnostic of the last scan, which Scan did not register.
		struct ScanFixSource
		{
			AssetScanFixKind Kind = AssetScanFixKind::AssignNewHandle;
			std::string Code{};
			std::string Path{};
			std::string Subject{};
			VfsPath MetaPath{};
			VfsPath SourcePath{};     // RenameMetaToSourceCase: the source's own spelling
			AssetMetadata Metadata{}; // AssignNewHandle: the duplicate .meta; RewriteImporter: the rewritten .meta
		};
	private:
		std::map<AssetHandle, AssetRecord> m_Records;   // by the .meta's handle (main assets and dependencies)
		std::map<VfsPath, AssetHandle> m_SourcePaths;   // source path -> handle
		std::map<AssetHandle, AssetHandle> m_SubAssets; // sub-asset handle -> its source's handle
		std::vector<AssetDiagnostic> m_ScanDiagnostics;
		std::vector<ScanFixSource> m_ScanFixes; // the auto-fixable diagnostics of the last scan
	};

}
