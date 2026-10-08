#include "EnginePCH.h"
#include "Engine/Renderer/PassBindingCache.h"

#include "Engine/Graphics/GraphicsDevice.h"

#include <algorithm>
#include <utility>

namespace Engine {

	Result<nvrhi::IBindingSet*> PassBindingCache::GetOrCreate(GraphicsDevice& device, const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout& layout)
	{
		// A view binds a handful of sets per pass, so a linear search is the cheapest lookup.
		const auto found = std::ranges::find_if(m_Entries, [&desc, &layout](const Detail::PassBindingCacheEntry& entry)
		{
			const nvrhi::BindingSetDesc* cached = entry.Set->getDesc();
			return entry.Set->getLayout() == &layout && cached != nullptr && *cached == desc;
		});
		if (found != m_Entries.end())
		{
			found->IsUsed = true;
			return found->Set.Get();
		}
		ENGINE_TRY_ASSIGN(nvrhi::BindingSetHandle set, device.CreateBindingSet(desc, layout));
		nvrhi::IBindingSet* created = set.Get();
		m_Entries.push_back({ .Set = std::move(set), .IsUsed = true });
		return created;
	}

	void PassBindingCache::ReleaseUnused()
	{
		std::erase_if(m_Entries, [](const Detail::PassBindingCacheEntry& entry)
		{
			return !entry.IsUsed;
		});
		for (Detail::PassBindingCacheEntry& entry : m_Entries)
			entry.IsUsed = false;
	}

	void PassBindingCache::Clear()
	{
		m_Entries.clear();
	}

}
