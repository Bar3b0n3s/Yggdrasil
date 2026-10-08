#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <vector>

// The binding sets one view records a pass with (Architecture §8.4, §8.14 item 1; Docs/Decisions/0013-m8-decisions.md
// decision 7). The passes that every view of a device shares (SkyboxPass, BloomPass, TonemapPass, FxaaPass, DebugRenderer
// and TextRenderer, created once per device and owned by SceneRendererPipelines) keep no binding sets of their own: each
// Record takes the caller's cache, and every SceneRenderer keeps one cache per pass beside its per-view targets. So views
// never evict each other's sets (no binding-set churn when several views render), and no pass keeps a view's targets or
// constant buffers alive after the view resized (SceneRenderer::Resize clears its caches) or was destroyed (its caches go
// with it).
//
// A set is found by its binding layout and its exact BindingSetDesc (NVRHI's equality: every item's resource, slot, type,
// format, dimension, subresources and range). A cached set holds references to its layout and its resources (NVRHI binding
// sets do), so no cached resource is destroyed while its set is cached and a recycled address can never match a stale set.
// ReleaseUnused drops the sets that were not used since its previous call; SceneRenderer calls it at the end of every
// Render, so a view keeps exactly what its last render bound, and an old environment cube, font atlas or blue-noise version
// is released one render after its last use (a command list that recorded a set keeps it alive until the GPU is done with
// it, §8.14 item 2). Implemented by the M8 contract; stream A owns it afterwards. Main thread only; not copyable.

namespace Engine {

	class GraphicsDevice;

	namespace Detail {

		// One cached set (at namespace scope: a nested struct with default member initializers is not default-constructible
		// inside its enclosing class on Clang with libstdc++).
		struct PassBindingCacheEntry
		{
			nvrhi::BindingSetHandle Set{};
			bool IsUsed = false; // returned by GetOrCreate since the previous ReleaseUnused
		};

	}

	class PassBindingCache
	{
	public:
		PassBindingCache() = default;
		~PassBindingCache() = default;

		PassBindingCache(const PassBindingCache&) = delete;
		PassBindingCache& operator=(const PassBindingCache&) = delete;
		PassBindingCache(PassBindingCache&&) noexcept = default;
		PassBindingCache& operator=(PassBindingCache&&) noexcept = default;

		// The cached set made from `desc` for `layout`, or a new one created through GraphicsDevice::CreateBindingSet and
		// cached; either way marked used. The pointer stays valid until the next ReleaseUnused or Clear (a command list that
		// recorded it keeps its own reference). Errors: those of GraphicsDevice::CreateBindingSet (Gpu); nothing is cached
		// then.
		[[nodiscard]] Result<nvrhi::IBindingSet*> GetOrCreate(GraphicsDevice& device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout& layout);

		// Drops every set that GetOrCreate did not return since the previous ReleaseUnused or Clear, and starts the next
		// period (the remaining sets count as unused until GetOrCreate returns them again).
		void ReleaseUnused();

		// Drops every set.
		void Clear();

		// The number of sets cached now.
		[[nodiscard]] size_t GetSize() const { return m_Entries.size(); }
	private:
		std::vector<Detail::PassBindingCacheEntry> m_Entries{};
	};

}
