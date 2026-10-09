#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"

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
	};
	using ThumbnailRenderer = std::function<Result<Image>(const ThumbnailRequest&)>;

	// Main-thread service. context outlives the cache; injected rendering owns its captures and never runs in a worker.
	// CPU tests inject images. Editor uses offscreen SceneRenderer for mesh/material/prefab/environment, decoded texture
	// data for textures, vector type icons otherwise. Memory/queue keys include project generation, canonical cache root,
	// asset/version/size and renderer-format version. Disk entries are confined to that bound cache root.
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
		[[nodiscard]] Status BindProject(uint64_t projectGeneration);
		[[nodiscard]] uint64_t GetProjectGeneration() const; // 0 unbound
		// Main thread outside renderer callbacks: cancels every queued request, drops results/errors and withdraws the
		// binding before mounts/private cache disappear. No disk deletion. Does not forget the last generation ever bound.
		void Reset();
		// NotFound unknown asset; InvalidArgument bad size/null handle; Conflict stale Version; Unsupported no renderer
		// for a visual preview; Io cache failures. Failure never replaces a valid cached PNG. No project source writes.
		// All Request/Queue/Find calls return InvalidState unbound or Conflict for a mismatched ProjectGeneration.
		// Pump validates the bound project/cache before any I/O; an old queued generation is dropped, never rendered.
		[[nodiscard]] Result<ThumbnailResult> Request(const ThumbnailRequest& request);
		// UI-side queue: no I/O/rendering. Deduplicates key, InvalidArgument malformed request. Pump runs at safe points,
		// processes at most maxItems (>0), retains each failure by key until its version changes, and never retries every frame.
		[[nodiscard]] Status Queue(const ThumbnailRequest& request);
		[[nodiscard]] Status Pump(uint32_t maxItems = 1);
		// Memory-only lookup: nullopt pending/unrequested, remembered render error on failure. No disk access during Draw.
		[[nodiscard]] Result<std::optional<ThumbnailResult>> Find(const ThumbnailRequest& request) const;
		// Drop entries and queued work for an asset in the CURRENT binding (all on invalid handle); disk entries remain.
		// Does not switch projects or replace Reset at project close.
		void Invalidate(AssetHandle asset = {});
	};

}
