#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace Engine {

	class EditorContext;

	struct ThumbnailRequest
	{
		uint64_t ProjectGeneration = 0; // nonzero binding from GetProjectGeneration; stale UI requests are Conflict
		AssetHandle Asset{};
		uint64_t Version = 0;
		uint32_t Size = 128; // square, [16, 512]
	};
	struct ThumbnailResult
	{
		std::string Path{}; // absolute PNG in the project's cache or private read-only cache
		bool Cached = false;
		bool TypeIcon = false; // true for types with no visual preview; no dummy PNG pretends to be rendered
		// Find only: an image of inputs that have changed since (the asset's version, or the version or load state of
		// anything it references). It stays displayable until its regeneration, which the caller requests with Queue.
		bool Stale = false;
	};
	using ThumbnailRenderer = std::function<Result<Image>(const ThumbnailRequest&)>;

	// Main-thread service. context outlives the cache; injected rendering owns its captures and never runs in a worker.
	// CPU tests inject images. Editor uses offscreen SceneRenderer for mesh/material/prefab/environment, decoded texture
	// data for textures, vector type icons otherwise. Memory/queue keys are the asset and size within the binding, which
	// supplies project generation, canonical cache root and renderer-format version.
	//
	// Disk entries live in <cache root>/Thumbnails and are content-keyed, so a later binding (a reopened project, another
	// editor session) reuses them: <asset>-<size>-<digest>.png, and the manifest <asset>-<size>.json lists the inputs
	// (the asset and everything it references through the asset dependency graph) whose files the digest covers: the
	// renderer format, EngineCookVersion, asset, size, for a captured image (mesh, material, prefab, environment) the
	// render quality (GetRenderQuality) and, for each input, the size and modification time of its source, its .meta and
	// its dependency files (built-ins contribute their handle). A reused image is valid only while recomputing that digest
	// from the current files and settings matches; in memory, a quality change marks captured images stale the way a
	// changed input does. Writing a new image removes the asset's previous one, and BindProject removes images no manifest
	// names, manifests of unregistered assets and the files of the earlier per-session naming, so the directory holds at
	// most one image per asset and size.
	//
	// Constructed unbound. The host binds after project open and resets BEFORE unmounting/releasing that project; it
	// also retires the old thumbnail ImGui texture keys. A queued request can never migrate to the next project.
	// Generation is explicit (Request), works headless with a GPU, and never occurs as a side effect of asset.list.
	class ThumbnailCache
	{
	public:
		ThumbnailCache(EditorContext& context, ThumbnailRenderer renderer);
		~ThumbnailCache();
		ThumbnailCache(const ThumbnailCache&) = delete;
		ThumbnailCache& operator=(const ThumbnailCache&) = delete;
		// Bind the current open project/cache. InvalidState without a project or while still bound (Reset first);
		// InvalidArgument generation 0 or a reused/non-increasing generation. Main-thread host allocates generations.
		// An open epoch uses a new generation even when reopening the same path. Invalid binding leaves state unchanged.
		// A successful binding sweeps the cache's Thumbnails directory (see the class comment); sweep failures only warn.
		[[nodiscard]] Status BindProject(uint64_t projectGeneration);
		[[nodiscard]] uint64_t GetProjectGeneration() const; // 0 unbound
		// The quality the renderer captures meshes, materials, prefabs and environments with: the open project's Rendering
		// settings (the defaults without a project). Captured images are keyed by it on disk and in memory.
		[[nodiscard]] RenderQualitySettings GetRenderQuality() const;
		// Main thread outside renderer callbacks: cancels every queued request, drops results/errors and withdraws the
		// binding before mounts/private cache disappear. No disk deletion. Does not forget the last generation ever bound.
		void Reset();
		// NotFound unknown asset; InvalidArgument bad size/null handle; Conflict stale Version; Unsupported no renderer
		// for a visual preview; Io cache failures. Failure never replaces a valid cached PNG. No project source writes.
		// All Request/Queue/Find calls return InvalidState unbound or Conflict for a mismatched ProjectGeneration.
		// Request generates synchronously (a reusable disk entry, or the renderer with the inputs as they are).
		[[nodiscard]] Result<ThumbnailResult> Request(const ThumbnailRequest& request);
		// UI-side queue: no I/O/rendering. Deduplicates asset and size (a newer Version replaces the queued one) and
		// ignores a request whose result is current; InvalidArgument malformed request.
		[[nodiscard]] Status Queue(const ThumbnailRequest& request);
		// Safe point: validates the bound project/cache before any I/O (an old queued generation is dropped, never
		// rendered) and visits each queued request at most once (at most 64, or maxItems, per call), producing at most
		// maxItems (>0) results. A request is processed at the asset's current version; one for an asset that is no
		// longer registered is dropped silently (deleting an asset is no error). A reusable disk entry or a type icon
		// needs no loading. Rendering needs the asset and its
		// references loaded: Pump never blocks on that, but starts the missing loads (AssetManager::LoadAsync) and keeps
		// the request queued while any of them is loading, so no render is discarded for a first load and no safe point
		// waits for unrelated imports. Returns the first render failure; each failure is remembered until an input changes.
		[[nodiscard]] Status Pump(uint32_t maxItems = 1);
		// Memory-only lookup for Draw: nullopt when unrequested, pending, or a remembered failure whose inputs changed
		// (queue it again); the remembered error while its inputs are unchanged; otherwise the last image, with Stale set
		// when its inputs changed. No disk access during Draw.
		[[nodiscard]] Result<std::optional<ThumbnailResult>> Find(const ThumbnailRequest& request) const;
		// Drop entries and queued work for an asset in the CURRENT binding (all on invalid handle); disk entries remain.
		// Does not switch projects or replace Reset at project close.
		void Invalidate(AssetHandle asset = {});
		// Marks every result of the current binding stale (an explicit Refresh): Find keeps returning them, Stale, until
		// queued requests recheck their disk inputs, which also catches files changed outside the editor while their
		// assets were not loaded. Memory only.
		void Revalidate();
	private:
		struct State;
		Scope<State> m_State;
	};

}
