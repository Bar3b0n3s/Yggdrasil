#include "TestsPCH.h"

#include "EditorCore/Graph/GraphLiveCache.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <utility>

// What graph.liveValues keeps of a graph between its calls (M24, C-VS; Docs/Decisions/0035-m24-contract.md decision 14; W24.2's
// review): a kept read holds for its key alone, one a graph, and the cache stays bounded. The method's use of it (an edit of
// the graph during play is read at once) is GraphMethodsTests'.

namespace Engine {

	namespace {

		GraphLiveCacheKey MakeKey(uint64_t graph, uint64_t sourceHash = 1, uint64_t version = 1, std::string path = "Assets/Scripts/Door.graph")
		{
			return GraphLiveCacheKey{ .Graph = AssetHandle(graph), .Path = std::move(path), .SourceHash = sourceHash, .Version = version };
		}

		GraphDocument MakeDocument(std::string name)
		{
			GraphDocument document;
			document.Name = std::move(name);
			return document;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("GraphLiveCache: a kept read holds for its key alone, one a graph, and past the limit the least recently used graph goes")
		{
			GraphLiveCache cache;
			CHECK(cache.Find(MakeKey(1)) == nullptr);
			GraphLiveCacheEntry& door = cache.Insert(MakeKey(1), MakeDocument("Door"));
			CHECK(door.Document.Name == "Door");
			CHECK_FALSE(door.LineMap.has_value());
			GraphLiveCacheEntry* found = cache.Find(MakeKey(1));
			REQUIRE(found != nullptr);
			CHECK(found == &door);

			// Any change of the graph misses: its bytes, its asset version or its path.
			CHECK(cache.Find(MakeKey(1, 2)) == nullptr);
			CHECK(cache.Find(MakeKey(1, 1, 2)) == nullptr);
			CHECK(cache.Find(MakeKey(1, 1, 1, "Assets/Doors/Door.graph")) == nullptr);
			// A new read of the graph replaces its entry, and the old key misses from then on.
			cache.Insert(MakeKey(1, 2), MakeDocument("Gate"));
			CHECK(cache.GetSize() == 1);
			CHECK(cache.Find(MakeKey(1)) == nullptr);
			REQUIRE(cache.Find(MakeKey(1, 2)) != nullptr);
			CHECK(cache.Find(MakeKey(1, 2))->Document.Name == "Gate");

			// Bounded: with every graph kept, a new one drops the least recently used other graph.
			for (uint64_t graph = 2; graph <= MaxGraphLiveCacheEntries; ++graph)
				cache.Insert(MakeKey(graph), MakeDocument("Other"));
			CHECK(cache.GetSize() == MaxGraphLiveCacheEntries);
			REQUIRE(cache.Find(MakeKey(1, 2)) != nullptr); // graph 1 is used again, so graph 2 is the least recently used
			cache.Insert(MakeKey(MaxGraphLiveCacheEntries + 1), MakeDocument("New"));
			CHECK(cache.GetSize() == MaxGraphLiveCacheEntries);
			CHECK(cache.Find(MakeKey(2)) == nullptr);
			CHECK(cache.Find(MakeKey(1, 2)) != nullptr);
			CHECK(cache.Find(MakeKey(MaxGraphLiveCacheEntries + 1)) != nullptr);

			cache.Clear();
			CHECK(cache.GetSize() == 0);
			CHECK(cache.Find(MakeKey(1, 2)) == nullptr);
		}
	}

}
