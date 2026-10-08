#include "EnginePCH.h"
#include "Engine/Renderer/Private/MaterialBindingCache.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"

#include <utility>

namespace Engine {

	namespace Utils {

		Result<nvrhi::IBindingSet*> MaterialBindingCache::GetOrCreate(GraphicsDevice& device, AssetHandle handle, const GpuMaterial& material,
			nvrhi::IBindingLayout& layout)
		{
			ENGINE_CORE_ASSERT(material.Constants != nullptr && material.BaseColorMap != nullptr && material.MetallicRoughnessMap != nullptr
					&& material.NormalMap != nullptr && material.OcclusionMap != nullptr && material.EmissiveMap != nullptr,
				"the material binding set of {} needs its constants and every map", handle);
			const auto found = m_Entries.find(handle);
			if (found != m_Entries.end() && found->second.Generation == material.Generation)
			{
				found->second.IsUsed = true;
				return found->second.Set.Get();
			}

			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(MaterialConstantsRegister, material.Constants),
				nvrhi::BindingSetItem::Texture_SRV(0, material.BaseColorMap),
				nvrhi::BindingSetItem::Texture_SRV(1, material.MetallicRoughnessMap),
				nvrhi::BindingSetItem::Texture_SRV(2, material.NormalMap),
				nvrhi::BindingSetItem::Texture_SRV(3, material.OcclusionMap),
				nvrhi::BindingSetItem::Texture_SRV(4, material.EmissiveMap),
			};
			ENGINE_TRY_ASSIGN(nvrhi::BindingSetHandle set, device.CreateBindingSet(desc, layout));
			nvrhi::IBindingSet* created = set.Get();
			m_Entries.insert_or_assign(handle, MaterialBindingEntry{ .Generation = material.Generation, .Set = std::move(set), .IsUsed = true });
			return created;
		}

		void MaterialBindingCache::ReleaseUnused()
		{
			std::erase_if(m_Entries, [](const auto& entry)
			{
				return !entry.second.IsUsed;
			});
			for (auto& [handle, entry] : m_Entries)
				entry.IsUsed = false;
		}

		void MaterialBindingCache::Clear()
		{
			m_Entries.clear();
		}

	}

}
