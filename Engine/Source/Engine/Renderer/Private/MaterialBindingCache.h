#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <map>

// The material binding sets of one view (§8.4 "one cached BindingSet per (material, version)"; SceneRenderer.h): the set-1
// set of each material mirror, keyed by the material's handle and the mirror's Generation (GpuResourceCache.h), which
// changes whenever the mirror's constants or textures do, so a cached set never holds an older texture. Like the shared
// passes' PassBindingCache, the view keeps the sets its last render used, and also those its last render kept: a material
// whose every draw the render culled keeps its set while it stays in the snapshot (Keep; M18, R-MATSET), so a material that
// leaves the view and returns makes no new set. SceneRenderer calls ReleaseUnused at the end of every Render, so an
// unloaded scene's materials, and through them their textures, are released one render after they left the snapshot.
// Main thread only; not copyable.

namespace Engine {

	class GraphicsDevice;
	struct GpuMaterial;

	namespace Utils {

		// One cached set (at namespace scope: a nested struct with default member initializers is not default-constructible
		// inside its enclosing class on Clang with libstdc++).
		struct MaterialBindingEntry
		{
			uint64_t Generation = 0;
			nvrhi::BindingSetHandle Set{};
			bool IsUsed = false; // returned by GetOrCreate, or kept by Keep, since the previous ReleaseUnused
		};

		class MaterialBindingCache
		{
		public:
			MaterialBindingCache() = default;
			~MaterialBindingCache() = default;

			MaterialBindingCache(const MaterialBindingCache&) = delete;
			MaterialBindingCache& operator=(const MaterialBindingCache&) = delete;

			// The set of `material` (the mirror of `handle`, whose constants and maps are not null; asserted) for the shared
			// set-1 `layout`: the cached one when it was made for the mirror's Generation, else a new one that replaces it;
			// marked used. The pointer stays valid until the next GetOrCreate for `handle`, ReleaseUnused or Clear. Errors:
			// those of GraphicsDevice::CreateBindingSet (Gpu); the previous set stays cached then.
			[[nodiscard]] Result<nvrhi::IBindingSet*> GetOrCreate(GraphicsDevice& device, AssetHandle handle, const GpuMaterial& material,
				nvrhi::IBindingLayout& layout);

			// Whether a set of `handle` is cached, of any generation.
			[[nodiscard]] bool Contains(AssetHandle handle) const;

			// Keeps the cached set of `handle` through the next ReleaseUnused without creating one (R-MATSET: the material's
			// draws were all culled): marks it used when it was made for `generation`, the current GpuMaterial::Generation of
			// the material's mirror. No effect when there is no set, or when the set holds an older generation's constants or
			// textures, which ReleaseUnused then drops so they are not kept alive.
			void Keep(AssetHandle handle, uint64_t generation);

			// Drops the sets neither GetOrCreate returned nor Keep kept since the previous ReleaseUnused or Clear, and starts
			// the next period.
			void ReleaseUnused();

			// Drops every set.
			void Clear();

			[[nodiscard]] size_t GetSize() const { return m_Entries.size(); }
		private:
			std::map<AssetHandle, MaterialBindingEntry> m_Entries{};
		};

	}

}
