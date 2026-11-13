#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

// What graph.liveValues keeps of a graph from one call to the next (M24, C-VS; Docs/Decisions/0035-m24-contract.md decision
// 14; W24.2's review): the Graph Editor and the inspector's graph slots poll the method once a frame during play, so instead of
// reading a graph's source strictly on every call and, while an error is retained, transpiling it again to map the error's
// line, the method keeps the document it read and its text's line map until the graph changes. An entry holds for its key,
// which every change of the graph changes: an edit, an undo or an outside write (the source's bytes), a move (its path) and a
// reimport that publishes a new version (the asset's version). The editor owns one (EditorContext::GetGraphLiveCache), so it
// is no global (Architecture §3 rule 5). Main thread; holds data only, no asset or VM reference.

namespace Engine {

	// What a kept read holds for.
	struct GraphLiveCacheKey
	{
		AssetHandle Graph{};
		std::string Path{};      // the source's project path ("Assets/Scripts/Door.graph")
		uint64_t SourceHash = 0; // the XXH64 of the source's bytes
		uint64_t Version = 0;    // the asset's version (AssetManager::GetVersion)

		bool operator==(const GraphLiveCacheKey&) const = default;
	};

	// One graph's kept read.
	struct GraphLiveCacheEntry
	{
		GraphLiveCacheKey Key{};
		GraphDocument Document{}; // the source read strictly
		// The graph's transpiled text's line map (its LineNodes; the text and the probes are not kept), made at the first error
		// to map; empty for a graph that does not transpile. FindGraphNodeAtLine reads it.
		std::optional<GraphTranspileResult> LineMap{};
		uint64_t LastUse = 0;
	};

	// The most graphs a cache keeps; past it the least recently used one goes.
	inline constexpr size_t MaxGraphLiveCacheEntries = 8;

	class GraphLiveCache
	{
	public:
		// The entry of `key`'s graph while it holds for `key` (a use), else null: a graph whose key changed is read again.
		[[nodiscard]] GraphLiveCacheEntry* Find(const GraphLiveCacheKey& key);
		// Keeps `document` for `key`, replacing the graph's entry and, past MaxGraphLiveCacheEntries graphs, dropping the least
		// recently used other one. The entry stays valid until its graph's entry is replaced or dropped, or the cache cleared.
		GraphLiveCacheEntry& Insert(GraphLiveCacheKey key, GraphDocument document);
		void Clear();
		[[nodiscard]] size_t GetSize() const { return m_Entries.size(); }
	private:
		std::map<AssetHandle, GraphLiveCacheEntry> m_Entries{};
		uint64_t m_Uses = 0; // the use counter LastUse is stamped from
	};

}
