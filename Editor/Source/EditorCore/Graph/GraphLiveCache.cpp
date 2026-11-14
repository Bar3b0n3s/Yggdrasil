#include "EditorPCH.h"
#include "EditorCore/Graph/GraphLiveCache.h"

#include <algorithm>
#include <utility>

// What graph.liveValues keeps of a graph between calls (Docs/Decisions/0035-m24-contract.md decision 14, W24.2's review).

namespace Engine {

	GraphLiveCacheEntry* GraphLiveCache::Find(const GraphLiveCacheKey& key)
	{
		const auto found = m_Entries.find(key.Graph);
		if (found == m_Entries.end() || found->second.Key != key)
			return nullptr;
		found->second.LastUse = ++m_Uses;
		return &found->second;
	}

	GraphLiveCacheEntry& GraphLiveCache::Insert(GraphLiveCacheKey key, GraphDocument document)
	{
		const AssetHandle graph = key.Graph;
		if (!m_Entries.contains(graph) && m_Entries.size() >= MaxGraphLiveCacheEntries)
		{
			const auto oldest = std::ranges::min_element(m_Entries, {}, [](const auto& entry)
			{
				return entry.second.LastUse;
			});
			m_Entries.erase(oldest);
		}
		GraphLiveCacheEntry& entry = m_Entries[graph];
		entry = GraphLiveCacheEntry{ .Key = std::move(key), .Document = std::move(document), .LineMap = std::nullopt, .LastUse = ++m_Uses };
		return entry;
	}

	void GraphLiveCache::Clear()
	{
		m_Entries.clear();
	}

}
