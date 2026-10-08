#pragma once

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/AssetPipeline/AssetDependencyGraph.h"
#include "Engine/AssetPipeline/AssetWriter.h"
#include "Engine/AssetPipeline/EngineAssetBaker.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UniqueFunction.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Platform/PollingFileWatcher.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The editor's asset manager (Architecture §3 rule 4, §7.2-§7.5): source -> import -> cook into the project cache ->
// load the cooked bytes with the same loaders the Runtime uses on pak entries (§7.2 "One load path"). It owns the
// registry (§7.3), the dependency graph (§7.5), the AssetWriter (no-echo writes) and, while a project is open, the hot
// reloader. EditorCore's EditorContext creates one for the editor's lifetime and injects it into the EngineContext.
//
// .meta files for new sources (§6.4, §7.3). The manager writes them in batches: OpenProject's and Refresh's scans, one
// poll's changes, and asset.import's copy. Within a batch it first resolves the dependency closure of every new or changed
// source (IAssetImporter::ListDependencyFiles; registered sources also through their dependency metas), then writes the
// metas: a source without a .meta that lies in a closure gets a dependency meta owned by that source (so a glTF's
// referenced image becomes a dependency, as §6.4 says, wherever it sorts in the batch relative to the glTF), every other
// source without a .meta gets its importer's asset meta, and only then do the batch's imports start, each with a lookup
// snapshot (ImportContext::Assets) taken after all of the batch's metas. A file that already has a .meta is never
// converted: an image that got its own Texture meta in an earlier batch stays a standalone texture, which a glTF that
// references it later reuses by handle (§6.4). Writes through the AssetWriter never create metas themselves: a source the
// editor writes without its .meta (asset.create and asset.import write both in one command) gets one at the next
// Refresh. After an import, every file it read that still has no .meta gets a dependency meta owned by the imported
// asset. A file it read that is another asset's dependency fails the import (ImportFailed naming the file and its owner,
// hint "give each glTF its own copy of the file"): a dependency has exactly one owner (AssetMetadata.h).
//
// Imports of one handle may overlap (a hot reload while an asset.reimport runs): every import carries a ticket
// (AssetHotReloader::BeginReimport) and only a completion whose ticket is still current is published and stored in the
// cache (§7.5 race rule 2); the manager serializes the cache stores of one handle, which AssetCache requires.
//
// Cooked bakes without a GPU (M8; §7.4, §7.5; Docs/Decisions/0013-m8-decisions.md decision 9). An environment's import is a
// GPU bake (EnvironmentImporter). Without an environment baker (--renderer none, tools), the manager serves it, before the
// importer is ever called and even for a reimport, from a cooked bake with the same cache key: the source's own project
// cache entry, then another source's (a copy of the same bytes, AssetCache::FindSourcesWithKey, in handle order), then the
// engine cooked cache's (a copy of a built-in HDRI), adopting only an import that read nothing but its source and produced
// just its main artifact. A bake taken from another entry is stored under the source's own handle too. Only when no cache
// holds one does the importer run and fail with Unsupported and the hint "start the editor with a GPU once to bake this
// environment" (ASSET_IMPORT_FAILED).
//
// Duplicated handles keep their original file (AssetRegistry::Scan's keeper rule). The manager passes the scan the
// locations of the last session, which it keeps in the local, gitignored <CacheRoot>/AssetLocations.json (canonical
// JSON {"Format": "AssetLocations", "Version": 1, "Assets": [{"Handle", "Path"}...]} sorted by handle, paths
// project-relative), rewritten after each scan from AssetRegistry::GetKnownLocations. A missing or unreadable file means
// no known locations (logged at Warn when unreadable); it is never committed, so it never conflicts in merges.

namespace Engine {

	class AssetHotReloader;
	class AssetLoaderRegistry;
	class EventLog;
	class IEnvironmentBaker;
	class IScriptDiagnosticsProvider;
	class ImporterRegistry;
	class MainThreadQueue;
	class TypeRegistry;
	class UUIDGenerator;
	class VirtualFileSystem;

	struct EditorAssetManagerSpecification
	{
		// The context's services and the editor's id generator (fresh handles for new .meta files, §4.8): documented
		// back-references that outlive the manager; never null. engine:// and enginecache:// are optional mounts (without
		// them only the procedural built-ins are available).
		VirtualFileSystem* Vfs = nullptr;
		JobSystem* Jobs = nullptr;
		MainThreadQueue* MainThread = nullptr;
		EventLog* Events = nullptr; // AssetReloaded and AssetImportFailed (§4.9)
		const TypeRegistry* Registry = nullptr;
		UUIDGenerator* IdGenerator = nullptr;
		const ImporterRegistry* Importers = nullptr;
		const AssetLoaderRegistry* Loaders = nullptr;
		// Injected by the editor: the Renderer's baker when a device exists (M8), the EditorCore type checker (M13).
		IEnvironmentBaker* EnvironmentBaker = nullptr;
		IScriptDiagnosticsProvider* ScriptDiagnostics = nullptr;
		// The generators of Generated built-ins (EngineAssetBaker.h; the Renderer's blue noise from M8), sorted by Id, for
		// GetOrBakeEngineAsset on first use; must outlive the manager. Empty: Generated built-ins load only from a bake run.
		std::span<const EngineAssetGenerator> EngineAssetGenerators{};
		// The importers are read from OpenProject on (never at construction), so a test may register an importer of its own
		// in `Importers` between constructing the manager and opening a project.
	};

	// How OpenProject treats the project.
	struct AssetProjectSpecification
	{
		VfsPath AssetsRoot{}; // "project://Assets"
		// "cache://": Library/Cache, or a read-only editor's private cache (§4.13); it also holds AssetLocations.json (see
		// the file comment).
		VfsPath CacheRoot{};
		// A read-only editor (--read-only, §4.13): nothing is written under the project. A source without a .meta is
		// registered with a transient in-memory meta whose handle is Hash64(0, its project-relative path), stable for the
		// session but never persisted; hot reload is off.
		bool ReadOnly = false;
		// Watch Assets/ for external changes (§7.5). Tests that drive the reloader themselves leave it on and call Update.
		bool HotReload = true;
	};

	// What a scan and the imports it triggered did. Lists sorted by handle (paths by path).
	struct AssetRefreshReport
	{
		size_t MetaCount = 0;                // .meta files read
		std::vector<VfsPath> CreatedMetas{}; // .meta files written for sources that had none (§7.3)
		std::vector<AssetHandle> Added{};    // registered now, not before
		std::vector<AssetHandle> Removed{};  // registered before, gone now
		std::vector<AssetHandle> Changed{};  // reimported because their source, a dependency file or a referenced asset changed
		// Changed, but held instead of reimported because reloads are deferred (race rule 4); reimported when the deferral
		// ends.
		std::vector<AssetHandle> Deferred{};
		std::vector<AssetDiagnostic> Diagnostics{}; // the scan's diagnostics and the (re)imports' failures and warnings
	};

	// What one import produced.
	struct AssetImportOutcome
	{
		AssetHandle Handle{};
		std::vector<SubAssetEntry> SubAssets{};     // as now written in the .meta
		std::vector<AssetDiagnostic> Diagnostics{}; // the importer's warnings
		bool FromCache = false;                     // served by a valid cache entry instead of the importer
	};

	// An external change of an asset file, as a poll of the hot reloader or a Refresh detected it (never the editor's own
	// writes, race rule 1). Each change is published once, by whichever of the two sees it first. EditorContext raises
	// SceneChangedOnDisk when it is the open scene or a prefab the open scene uses (race rule 3).
	struct AssetExternalChange
	{
		VfsPath Path{};       // the source file
		AssetHandle Handle{}; // its main asset; null for a file that was not registered (created)
		AssetType Type = AssetType::None;
		FileChangeKind Kind = FileChangeKind::Modified;
	};

	// Main thread only (§4.11); imports, cache reads and decodes run on the JobSystem, and their results are published on the
	// main thread (MainThreadQueue::Drain). With an inline JobSystem (tests) every job runs inside the call that submits it,
	// so behaviour is deterministic. Not copyable.
	class EditorAssetManager final : public AssetManager
	{
	public:
		using ExternalChangeListener = UniqueFunction<void(const AssetExternalChange& change)>;
		using ReloadListener = UniqueFunction<void(AssetHandle source)>;

		explicit EditorAssetManager(const EditorAssetManagerSpecification& specification);
		// Closes the project (waiting for its jobs).
		~EditorAssetManager() override;

		// --- AssetManager ----------------------------------------------------------------------------------------------

		// Loads from the project cache when it holds a valid entry for the source's current key and manifest, else imports
		// (on the calling thread for Load; RequiresMainThread importers always on the main thread), stores the result in the
		// cache and loads the cooked bytes. File and Generated built-ins come from the engine cooked cache
		// (GetOrBakeEngineAsset). A failure is remembered as AssetManager::Load describes: the source is not imported again
		// until it, a dependency file, its settings or its importer changes.
		[[nodiscard]] Result<AssetRef<Asset>> Load(AssetHandle handle) override;
		[[nodiscard]] JobHandle<AssetRef<Asset>> LoadAsync(AssetHandle handle) override;
		[[nodiscard]] AssetState GetState(AssetHandle handle) const override;
		[[nodiscard]] const AssetMetadata* GetMetadata(AssetHandle handle) const override;
		[[nodiscard]] AssetType GetAssetType(AssetHandle handle) const override;
		[[nodiscard]] std::optional<AssetHandle> Resolve(std::string_view reference) const override;
		[[nodiscard]] std::string GetReferencePath(AssetHandle handle) const override;
		void WaitIdle() override;

		// --- Projects --------------------------------------------------------------------------------------------------

		// Scans `project.AssetsRoot` (AssetRegistry::Scan with the importers' descriptions and the known locations of
		// AssetLocations.json), writes a .meta for every source without one as one batch (the closure rule of the file
		// comment; writable projects, through the AssetWriter; a read-only project registers a transient meta in memory),
		// records the scan diagnostics, rewrites AssetLocations.json, and starts hot reload. Nothing is imported until loaded
		// or refreshed. Errors: InvalidState when a project is open; the scan's errors; the .meta writes' errors (the project
		// stays closed then).
		[[nodiscard]] Result<AssetRefreshReport> OpenProject(const AssetProjectSpecification& project);

		// Stops hot reload, waits for the project's jobs, and forgets its registry, graph, cache and loaded project assets
		// (built-ins stay). No effect without a project.
		void CloseProject();

		[[nodiscard]] bool HasProject() const;

		// --- Registry, graph, built-ins (read) --------------------------------------------------------------------------

		[[nodiscard]] const AssetRegistry& GetRegistry() const;
		[[nodiscard]] const AssetDependencyGraph& GetDependencyGraph() const;
		// engine://EngineAssets.json when engine:// is mounted (read at construction; a failure is logged at Error and leaves
		// the procedural entries), otherwise the procedural entries (GetProceduralBuiltinEntries).
		[[nodiscard]] const BuiltinAssetCatalog& GetBuiltins() const;

		// --- Refresh and imports ---------------------------------------------------------------------------------------

		// project.refreshAssets (§7.3), and the implicit refresh before every automation call that takes a path: rescans
		// synchronously, writes .meta files for new sources (one batch, the closure rule of the file comment), imports every
		// main asset the rescan registered that was never imported (so its .meta lists its sub-assets and its import
		// problems are reported now; a poll's batch starts the same imports on jobs), reimports every asset whose source,
		// dependency files or referenced assets changed since its last import (and their dependents, in
		// AssetDependencyGraph::GetReimportOrder), publishes the new versions and returns after all of it (WaitIdle), so a
		// just-written file never races the watcher.
		//
		// Every external change Refresh detects takes the same path as one a poll detects, so §7.5's race rules hold
		// whichever observer sees it first: the path is marked known on the hot reloader's watcher (PollingFileWatcher::
		// MarkKnown, so the watcher never reports it again; a poll the watcher already computed, delivered at a later frame,
		// is dropped because the manager's known state of the file already holds it), the change reaches the external-change
		// listener once (race rule 3: SceneChangedOnDisk is raised now, not at a later poll), and the asset is reimported and
		// published once (one version bump and one AssetReloaded event, as for a poll; dependents follow). While reloads
		// are deferred (race rule 4), Refresh still rescans and writes the new sources' metas but reimports nothing: the
		// changed assets are listed in Deferred and held with the hot reloader's held changes, and the listener hears of
		// them when the deferral ends. Errors: InvalidState without a project; the scan's errors. Failed reimports are
		// reported in the result and as diagnostics, not as errors.
		[[nodiscard]] Result<AssetRefreshReport> Refresh();

		// Imports the main asset `handle` again, bypassing the cache, rewrites its .meta when its sub-assets changed, and
		// publishes the result (a new version of every artifact that changed). Synchronous. Errors: InvalidState without a
		// project; NotFound; InvalidArgument for a sub-asset, dependency or built-in handle; the import's errors (the last good
		// version stays in use and the failure is recorded as a diagnostic, §7.5).
		[[nodiscard]] Result<AssetImportOutcome> Reimport(AssetHandle handle);

		// Reimport on a job, published on the main thread (asset.reimport is a pending operation, §13.2). Errors in the handle:
		// as Reimport.
		[[nodiscard]] JobHandle<AssetImportOutcome> ReimportAsync(AssetHandle handle);

		// A new .meta for `source` (not written): a fresh handle from the id generator, the importer that takes its extension,
		// that importer's default settings, no sub-assets. asset.create and asset.import write it with the source in one
		// command. Errors: NotFound when no importer takes the extension (the hint lists the importable extensions).
		[[nodiscard]] Result<AssetMetadata> CreateMetadata(const VfsPath& source) const;

		// A dependency meta (§6.4) owned by `owner`, with a fresh handle from the id generator (not written): asset.import
		// writes one for each file of a glTF's dependency closure (§13.2).
		[[nodiscard]] AssetMetadata CreateDependencyMetadata(AssetHandle owner) const;

		// The main assets whose last import found `handle` by its source path (ImportContext::FindAsset; a glTF that reuses a
		// standalone texture, §6.4), sorted. Such a reference follows the path, not the handle, so asset.move refuses to move
		// `handle` away from it (AssetMoveCommand) and asset.delete warns.
		[[nodiscard]] std::vector<AssetHandle> GetPathDependents(AssetHandle handle) const;

		// The complete canonical settings of importer `importerId` from the merge patch `patch` over `base` (both JSON
		// objects; null `base` means the importer's defaults): asset.setImportSettings. Errors: NotFound for an unknown
		// importer; Validation (located, issues relative to the settings root) for an invalid result; InvalidArgument for an
		// importer without settings.
		[[nodiscard]] Result<VariantValue> MergeImportSettings(std::string_view importerId, const VariantValue& base, const Json& patch) const;

		// --- Writes ----------------------------------------------------------------------------------------------------

		// The editor's no-echo writer (§7.5 race rule 1). EditorContext's project write path goes through it; its events update
		// the registry at once (a written .meta is registered, re-registered or removed synchronously, so asset.create can
		// report the new handle) and schedule reimports of written sources.
		[[nodiscard]] AssetWriter& GetWriter();

		using WriteObserver = UniqueFunction<void(const AssetWriteEvent& event)>;

		// Receives every event of the manager's AssetWriter, after the manager processed it: the editor's own write path
		// (EditorContext::WriteProjectFile, MoveProjectFile, RemoveProjectFile, CreateProjectDirectory) and the manager's own
		// writes (the .meta files OpenProject, Refresh and hot reload write for new sources, the dependency metas from import
		// reads, and the .meta rewrite after an import changes the sub-assets). EditorContext keeps provenance with it
		// (§13.12 rule 1): Record with the current attribution for a written or moved-in file under Assets/, Remove for a
		// removed or moved-out one; nothing during a dry run or in a read-only editor. Empty: none.
		void SetWriteObserver(WriteObserver observer);

		// --- Hot reload (§7.5) -----------------------------------------------------------------------------------------

		// Once per frame from EditorContext::Update with a monotonic time in seconds: drives the hot reloader's polling. Each
		// external change reimports the asset on a job (with a ticket, race rule 2), and the swap is published at the start
		// of a later frame (MainThreadQueue::Drain): the version bumps, an AssetReloaded event is appended, dependents are
		// reimported, and the external-change listener is called. A failed reimport keeps the last good version and records
		// a diagnostic plus an AssetImportFailed event. No effect without a project or with hot reload off.
		void Update(double nowSeconds);

		// Race rule 4: while true, external changes (from polls and from Refresh) are held and nothing is reimported; false
		// applies the held changes, merged per path, and publishes them.
		void SetReloadsDeferred(bool deferred);
		[[nodiscard]] bool AreReloadsDeferred() const;

		// Receives every published external change, whether a poll or Refresh detected it, once (race rule 3 is the
		// listener's: EditorContext). Empty: none.
		void SetExternalChangeListener(ExternalChangeListener listener);

		// Receives every published reload: a reimport of a main asset already published that changed what it cooks (when the
		// AssetReloaded event is appended), whatever started it: an external change (a poll or Refresh), the editor's own
		// write of the source or its .meta (EditorContext::WriteProjectFile: asset.setProperties, asset.import, script.write),
		// Reimport. Not during a dry run, which publishes nothing that stays, and not while reloads are deferred (they are
		// published when the deferral ends). EditorContext marks a running play session modified with it (§7.5 race rule 4).
		// Empty: none.
		void SetReloadListener(ReloadListener listener);

		// The hot reloader of the open project, or nullptr (no project, read-only, or hot reload off). For tests and
		// diagnostics.
		[[nodiscard]] AssetHotReloader* GetHotReloader();

		// --- Dry runs (§13.4) ------------------------------------------------------------------------------------------

		// Called by EditorDryRunScope after it swapped project:// for an overlay: snapshots the registry, the graph and the
		// loaded-asset table, saves the base's versions and diagnostics (AssetManager::SaveSharedState), switches the writer
		// to dry-run mode and makes imports run in memory only (no cache writes, no jobs left running, no events), so
		// registry updates and their results are real but leave no trace. Asserts no dry run is open.
		void BeginDryRun();
		// Restores the snapshot (AssetManager::RestoreSharedState: versions included) and leaves dry-run mode. Asserts an open
		// dry run.
		void EndDryRun();
		[[nodiscard]] bool IsDryRun() const;
	private:
		// The specification, the catalogue, the project (registry, graph, cache, hot reloader, writer), the loaded-asset table
		// with states and tickets, and the dry-run snapshot (EditorAssetManager.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
