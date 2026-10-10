#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The asset manager interface (Architecture §3 rule 4, §7.2), frozen by the M6 contract. EditorAssetManager (AssetPipeline:
// source -> cook -> load) and RuntimeAssetManager (Asset: paks) implement it; EditorApp and RuntimeApp inject theirs into
// EngineContext as the context's AssetManager (EngineContext::SetAssetManager). Engine code below AssetPipeline (Scene,
// Renderer, Session, Scripting) only ever sees this interface.

namespace Engine {

	// Where an asset is in its life (§7.2).
	enum class AssetState : uint8_t
	{
		Unloaded, // registered, not loaded (or released)
		Loading,  // a LoadAsync (or an import it needs) is running
		Loaded,   // the current version is loaded
		Failed    // the last load failed; GetOrPlaceholder serves the placeholder and a diagnostic is recorded
	};

	// "Unloaded", "Loading", "Loaded" or "Failed".
	[[nodiscard]] std::string_view AssetStateToString(AssetState state);

	// The common interface and the shared failure policy of both managers (§7.2):
	//   - Loaded assets are immutable AssetRef<T>, safe to share across threads. A hot reload replaces the manager's entry
	//     and bumps the asset's version; references handed out before stay valid until released. GpuResourceCache keys its
	//     mirrors by (handle, GetVersion(handle)).
	//   - One load path: both managers decode cooked bytes with the same IAssetLoader (AssetLoaderRegistry).
	//   - A missing or failed asset never yields null through GetOrPlaceholder: the caller gets the type's placeholder
	//     (GetPlaceholderHandle: Cube, Missing, Error, Default font, the silent clip), the problem is logged once and an AssetDiagnostic is
	//     recorded (GetDiagnostics; the editor, "_meta" through the log, project.validate). Export refuses to run while any
	//     diagnostic has severity Error.
	//   - The procedural built-ins (GetProceduralBuiltinEntries) are served by this base class without loading anything, so
	//     the mesh, texture and material placeholders can never fail.
	//
	// Thread safety: main thread only (§4.11), except where a member says otherwise; LoadAsync decodes on the JobSystem and
	// publishes on the main thread (MainThreadQueue::Drain). Not copyable or movable.
	class AssetManager
	{
	public:
		virtual ~AssetManager();

		AssetManager(const AssetManager&) = delete;
		AssetManager& operator=(const AssetManager&) = delete;
		AssetManager(AssetManager&&) = delete;
		AssetManager& operator=(AssetManager&&) = delete;

		// --- The interface of §7.2 ------------------------------------------------------------------------------------

		// The asset `handle` at its current version, loading it synchronously when needed (in the editor: importing and
		// cooking first when the cache has no valid entry, §7.2 "One load path"). A procedural built-in is served directly.
		// Errors: InvalidArgument for the null handle or a dependency's handle (§6.4: never loaded on its own); NotFound for
		// an unknown handle; ImportFailed, Parse, Validation, UnsupportedVersion or Io from the import, the cache or the
		// loader; Unsupported for a type without a loader in this build. Each error
		// names the handle and its path.
		//
		// Failures are remembered, so repeated calls (GetOrPlaceholder from renderer lookups, a batch of prefab.instantiate
		// ops) never import a broken source again and again: when a previous version is loaded, a failed reimport keeps it in
		// use (Load returns it, the state stays Loaded) and records the error as a diagnostic; otherwise the state is Failed
		// and Load returns the recorded error at once. Either way nothing is imported again for that handle until its
		// source, one of its dependency files, its settings or its importer changes (hot reload, Refresh, Reimport).
		[[nodiscard]] virtual Result<AssetRef<Asset>> Load(AssetHandle handle) = 0;

		// Starts loading `handle` without blocking: the import or cache read and the decode run on jobs, and the result is
		// published to the manager on the main thread, after which GetState is Loaded or Failed. The handle resolves with
		// the same value or error as Load. Loading an asset that is already loaded completes at once.
		[[nodiscard]] virtual JobHandle<AssetRef<Asset>> LoadAsync(AssetHandle handle) = 0;

		// Unloaded for an unknown handle.
		[[nodiscard]] virtual AssetState GetState(AssetHandle handle) const = 0;

		// The metadata of the .meta that defines `handle` (for a sub-asset, its source's .meta; for an engine asset, nullptr),
		// or nullptr for an unknown handle. Valid until the next registry change (a scan, a write, a reimport).
		[[nodiscard]] virtual const AssetMetadata* GetMetadata(AssetHandle handle) const = 0;

		// The type of `handle` (a main asset, a sub-asset or a built-in); None for an unknown handle or a dependency.
		[[nodiscard]] virtual AssetType GetAssetType(AssetHandle handle) const = 0;

		// The handle an AssetReference names (§7.1): a 16-hex handle this manager knows, a project path or sub-asset path of a
		// registered asset, or an engine path of a built-in. nullopt when nothing matches (case-sensitive, §4.10).
		[[nodiscard]] virtual std::optional<AssetHandle> Resolve(std::string_view reference) const = 0;

		// The readable reference responses carry as "path" (§7.1: references expand to {id, path, type}):
		// "Assets/Models/Track.glb#mesh:0:Straight", "engine://Meshes/Cube"; empty for an unknown handle.
		[[nodiscard]] virtual std::string GetReferencePath(AssetHandle handle) const = 0;

		// Blocks until every load, import and hot-reload swap requested so far has completed and been published (draining the
		// main-thread work this manager posted), so screenshots and tests see a settled state (§8.13). Main thread.
		virtual void WaitIdle() = 0;

		// --- The shared failure policy (§7.2) --------------------------------------------------------------------------

		// The asset `handle` as a T, or T's placeholder (never null) when it is missing, failed to load or is of another type.
		// The first failure of each (handle, problem) is logged (Error) and recorded as a diagnostic: AssetMissingCode for an
		// unknown handle, AssetTypeMismatchCode for an asset of another type, AssetImportFailedCode for a failed load. A null
		// handle yields the placeholder without a diagnostic (callers that treat null as "nothing" check it first). T must
		// have a placeholder (GetPlaceholderHandle(T::StaticType) is valid).
		//
		// AssetMissingCode and AssetTypeMismatchCode recorded here are reference diagnostics: they report a use that failed,
		// not the state of an asset, so they never count for HasErrorDiagnostics (play and export check the current
		// references instead: ProjectValidator reports ASSET_MISSING and ASSET_TYPE_MISMATCH for references that are wrong
		// now). AssetMissingCode of a handle is cleared as soon as the handle is registered or loads (a rescan, a write, the
		// undo of a delete); AssetTypeMismatchCode of a handle is cleared at the next registry scan (OpenProject, Refresh),
		// after which a use that is still wrong records it again.
		template<AssetDataType T>
		[[nodiscard]] AssetRef<T> GetOrPlaceholder(AssetHandle handle)
		{
			if (handle.IsValid())
			{
				Result<AssetRef<Asset>> loaded = Load(handle);
				if (loaded.has_value())
				{
					if (AssetRef<T> typed = AssetCast<T>(*loaded))
						return typed;
					ReportUnavailable(handle, T::StaticType, nullptr);
				}
				else
				{
					ReportUnavailable(handle, T::StaticType, &loaded.error());
				}
			}
			return AssetCast<T>(GetPlaceholder(T::StaticType));
		}

		// The placeholder of `type` (GetPlaceholderHandle), never null: the procedural built-ins for Mesh, Texture and
		// Material; for Font the Default font when it loads, else an empty FontData (no glyphs), with the failure reported
		// once. Asserts a type that has a placeholder.
		[[nodiscard]] AssetRef<Asset> GetPlaceholder(AssetType type);

		// The version of `handle`: 0 until it first loads, 1 after the first load, +1 for every hot reload that replaces it
		// (§7.2, §7.5). Procedural built-ins are at 1 from their first use.
		[[nodiscard]] uint64_t GetVersion(AssetHandle handle) const;

		// Every diagnostic currently recorded, sorted by (Path, Code, Subject); the registry's scan diagnostics included in the
		// editor. Valid until the next change of the diagnostics.
		[[nodiscard]] std::span<const AssetDiagnostic> GetDiagnostics() const;

		// True when a recorded diagnostic that describes the state of an asset has severity Error: ASSET_IMPORT_FAILED and
		// the scan's errors (export refuses to run, §7.2; play is blocked, §12.4). The reference diagnostics of
		// GetOrPlaceholder and the runtime-only ASSET_UPLOAD_FAILED (a GPU failure of this process, not a property of the
		// project) are not counted. A handle's diagnostics are removed when it is unregistered.
		[[nodiscard]] bool HasErrorDiagnostics() const;

		// Records `diagnostic` (replacing an equal one) and logs it once per (Code, Asset, Subject): Error severity at Error,
		// Warning at Warn, through the Engine logger. Renderer/GpuResourceCache reports upload failures here (§8.14 item 7).
		void ReportDiagnostic(AssetDiagnostic diagnostic);
	protected:
		AssetManager();

		// Marks a new version of `handle` published (a hot-reload swap, or its first load).
		void BumpVersion(AssetHandle handle);

		// Removes every diagnostic recorded for `handle` whose code is one of `codes` (a successful reimport clears the asset's
		// import diagnostics) and allows those problems to be logged again.
		void ClearDiagnostics(AssetHandle handle, std::span<const std::string_view> codes);

		// Replaces the registry's scan diagnostics (the editor, after each scan), which GetDiagnostics merges in.
		void SetScanDiagnostics(std::vector<AssetDiagnostic> diagnostics);

		// The procedural built-in `handle`, created once and cached (CreateProceduralBuiltinAsset). Errors: NotFound when it is
		// not one.
		[[nodiscard]] Result<AssetRef<Asset>> GetProceduralBuiltin(AssetHandle handle);

		// Dry runs (EditorAssetManager::BeginDryRun, §13.4 "leaves no trace"): SaveSharedState keeps a copy of the versions,
		// the diagnostics (scan diagnostics included) and the logged-once set, and RestoreSharedState puts that copy back and
		// drops it, so the version bumps, loads and diagnostics of a dry run are undone exactly (versions included, which
		// otherwise only ever grow). One copy at a time: SaveSharedState asserts none is kept, RestoreSharedState asserts one
		// is. The procedural built-in cache is not part of it (built-ins never change).
		void SaveSharedState();
		void RestoreSharedState();
	private:
		// GetOrPlaceholder's report: the diagnostic for an unknown handle (error NotFound), a type mismatch (`error` null) or
		// a failed load, logged once.
		void ReportUnavailable(AssetHandle handle, AssetType expectedType, const Error* error);
	private:
		// Versions, diagnostics, the logged-once set, the procedural built-in cache and the dry run's saved copy
		// (AssetManager.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
