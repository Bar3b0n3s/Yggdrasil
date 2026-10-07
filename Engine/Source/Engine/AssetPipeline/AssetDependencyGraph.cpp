#include "EnginePCH.h"
#include "Engine/AssetPipeline/AssetDependencyGraph.h"

#include <algorithm>

namespace Engine {

	void AssetDependencyGraph::SetDependencies(AssetHandle asset, std::span<const AssetHandle> dependencies)
	{
		RemoveAsset(asset);
		std::set<AssetHandle> edges;
		for (const AssetHandle dependency : dependencies)
		{
			if (dependency != asset && dependency.IsValid())
				edges.insert(dependency);
		}
		if (edges.empty())
			return;
		for (const AssetHandle dependency : edges)
			m_Dependents[dependency].insert(asset);
		m_Dependencies.emplace(asset, std::move(edges));
	}

	void AssetDependencyGraph::RemoveAsset(AssetHandle asset)
	{
		const auto found = m_Dependencies.find(asset);
		if (found == m_Dependencies.end())
			return;
		for (const AssetHandle dependency : found->second)
		{
			const auto dependents = m_Dependents.find(dependency);
			if (dependents == m_Dependents.end())
				continue;
			dependents->second.erase(asset);
			if (dependents->second.empty())
				m_Dependents.erase(dependents);
		}
		m_Dependencies.erase(found);
	}

	void AssetDependencyGraph::Clear()
	{
		m_Dependencies.clear();
		m_Dependents.clear();
	}

	std::vector<AssetHandle> AssetDependencyGraph::GetDependencies(AssetHandle asset) const
	{
		const auto found = m_Dependencies.find(asset);
		if (found == m_Dependencies.end())
			return {};
		return { found->second.begin(), found->second.end() };
	}

	std::vector<AssetHandle> AssetDependencyGraph::GetDependents(AssetHandle asset) const
	{
		const auto found = m_Dependents.find(asset);
		if (found == m_Dependents.end())
			return {};
		return { found->second.begin(), found->second.end() };
	}

	std::vector<AssetHandle> AssetDependencyGraph::GetTransitiveDependents(AssetHandle asset) const
	{
		std::set<AssetHandle> reached;
		std::vector<AssetHandle> pending = { asset };
		while (!pending.empty())
		{
			const AssetHandle current = pending.back();
			pending.pop_back();
			const auto found = m_Dependents.find(current);
			if (found == m_Dependents.end())
				continue;
			for (const AssetHandle dependent : found->second)
			{
				if (dependent != asset && reached.insert(dependent).second)
					pending.push_back(dependent);
			}
		}
		return { reached.begin(), reached.end() };
	}

	std::vector<AssetHandle> AssetDependencyGraph::GetReimportOrder(std::span<const AssetHandle> changed) const
	{
		// The affected set: the changed assets and everything that references them, directly or not.
		std::set<AssetHandle> affected;
		for (const AssetHandle asset : changed)
		{
			affected.insert(asset);
			for (const AssetHandle dependent : GetTransitiveDependents(asset))
				affected.insert(dependent);
		}

		// Kahn's algorithm within the affected set, the smallest ready handle first.
		std::map<AssetHandle, size_t> pendingDependencies;
		for (const AssetHandle asset : affected)
		{
			size_t count = 0;
			const auto found = m_Dependencies.find(asset);
			if (found != m_Dependencies.end())
			{
				count = static_cast<size_t>(std::ranges::count_if(found->second, [&affected](AssetHandle dependency)
				{
					return affected.contains(dependency);
				}));
			}
			pendingDependencies.emplace(asset, count);
		}

		std::set<AssetHandle> ready;
		for (const auto& [asset, count] : pendingDependencies)
		{
			if (count == 0)
				ready.insert(asset);
		}

		std::vector<AssetHandle> order;
		order.reserve(affected.size());
		while (!ready.empty())
		{
			const AssetHandle next = *ready.begin();
			ready.erase(ready.begin());
			order.push_back(next);
			pendingDependencies.erase(next);
			const auto dependents = m_Dependents.find(next);
			if (dependents == m_Dependents.end())
				continue;
			for (const AssetHandle dependent : dependents->second)
			{
				const auto pending = pendingDependencies.find(dependent);
				if (pending != pendingDependencies.end() && --pending->second == 0)
					ready.insert(dependent);
			}
		}

		// What is left lies on a reference cycle (script requires): after the acyclic part, sorted by handle.
		for (const auto& entry : pendingDependencies)
			order.push_back(entry.first);
		return order;
	}

}
