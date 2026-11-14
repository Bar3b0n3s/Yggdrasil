#pragma once

#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// The graph canvas's node menu (M24, W24.2's stream A; Docs/Decisions/0035-m24-contract.md decision 15): the kinds graph.nodes
// lists for the open graph, searchable and by category, and, for a link dropped on empty canvas (AddLinkedNode), only the kinds
// with a pin the link can join. UI-free and pure.

namespace Engine {

	// One entry of the menu: a kind, and for a dropped link the pin of that kind the link joins.
	struct GraphNodeMenuItem
	{
		const GraphNodeDescription* Node = nullptr; // borrowed from the span FilterGraphNodeMenu was given
		std::string Pin{};                          // with a link: the new node's pin to connect; empty otherwise
	};

	// The menu's caption of `category` ("Events", "Flow", "Members", "Operators", "Calls", "Properties", "Constants"); empty
	// for a value outside the enumeration. Pure.
	[[nodiscard]] std::string_view GetGraphNodeCategoryLabel(GraphNodeCategory category);

	// The entries of `nodes` the menu lists, in category order (the enumeration's), then by kind byte-wise:
	//   - `search`: every space-separated word of it occurs, ignoring ASCII case, in the kind or the title; empty lists every kind.
	//   - `link` (a pin of the node a dropped link started from, or nullptr): only the kinds with a pin of the other direction
	//     that it joins (AreGraphPinsCompatible, from the output to the input), each entry naming its first such pin in the
	//     kind's pin order.
	// Pure.
	[[nodiscard]] std::vector<GraphNodeMenuItem> FilterGraphNodeMenu(std::span<const GraphNodeDescription> nodes, std::string_view search, const GraphPinDescription* link);

}
