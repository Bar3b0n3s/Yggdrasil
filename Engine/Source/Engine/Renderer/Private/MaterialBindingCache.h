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
// passes' PassBindingCache, the view keeps exactly the sets its last render used: SceneRenderer calls ReleaseUnused at the
// end of every Render, so an unloaded scene's materials, and through them their textures, are released one render after
// their last use. Main thread only; not copyable.

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
			bool IsUsed = false; // returned by GetOrCreate since the previous ReleaseUnused
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

			// Drops the sets GetOrCreate did not return since the previous ReleaseUnused or Clear, and starts the next period.
			void ReleaseUnused();

			// Drops every set.
			void Clear();

			[[nodiscard]] size_t GetSize() const { return m_Entries.size(); }
		private:
			std::map<AssetHandle, MaterialBindingEntry> m_Entries{};
		};

	}

}
