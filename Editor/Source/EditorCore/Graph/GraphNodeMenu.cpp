#include "EditorPCH.h"
#include "EditorCore/Graph/GraphNodeMenu.h"

#include <algorithm>
#include <tuple>
#include <utility>

// The graph canvas's node menu (Docs/Decisions/0035-m24-contract.md decision 15, W24.2's stream A).

namespace Engine {

	namespace Utils {

		static char LowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
		}

		// Whether `word` occurs in `text`, ignoring ASCII case.
		static bool ContainsIgnoringCase(std::string_view text, std::string_view word)
		{
			if (word.empty())
				return true;
			const auto found = std::ranges::search(text, word, [](char left, char right)
			{
				return LowerAscii(left) == LowerAscii(right);
			});
			return !found.empty();
		}

		// Whether every space-separated word of `search` occurs in the kind or the title of `node`.
		static bool MatchesSearch(const GraphNodeDescription& node, std::string_view search)
		{
			while (!search.empty())
			{
				const size_t space = search.find(' ');
				const std::string_view word = search.substr(0, space);
				search = space == std::string_view::npos ? std::string_view{} : search.substr(space + 1);
				if (!word.empty() && !ContainsIgnoringCase(node.Kind, word) && !ContainsIgnoringCase(node.Title, word))
					return false;
			}
			return true;
		}

		// The first pin of `node` that `link` joins, from the output to the input; empty when none does.
		static std::string FindLinkedPin(const GraphNodeDescription& node, const GraphPinDescription& link)
		{
			for (const GraphPinDescription& pin : node.Pins)
			{
				if (pin.Direction == link.Direction)
					continue;
				const bool joins = link.Direction == GraphPinDirection::Output ? AreGraphPinsCompatible(link, pin) : AreGraphPinsCompatible(pin, link);
				if (joins)
					return pin.Name;
			}
			return {};
		}

	}

	std::string_view GetGraphNodeCategoryLabel(GraphNodeCategory category)
	{
		switch (category)
		{
			case GraphNodeCategory::Event:    return "Events";
			case GraphNodeCategory::Flow:     return "Flow";
			case GraphNodeCategory::Member:   return "Members";
			case GraphNodeCategory::Operator: return "Operators";
			case GraphNodeCategory::Call:     return "Calls";
			case GraphNodeCategory::Property: return "Properties";
			case GraphNodeCategory::Constant: return "Constants";
		}
		return {};
	}

	std::vector<GraphNodeMenuItem> FilterGraphNodeMenu(std::span<const GraphNodeDescription> nodes, std::string_view search, const GraphPinDescription* link)
	{
		std::vector<GraphNodeMenuItem> items;
		for (const GraphNodeDescription& node : nodes)
		{
			if (!Utils::MatchesSearch(node, search))
				continue;
			std::string pin;
			if (link != nullptr)
			{
				pin = Utils::FindLinkedPin(node, *link);
				if (pin.empty())
					continue;
			}
			items.push_back({ .Node = &node, .Pin = std::move(pin) });
		}
		std::ranges::sort(items, [](const GraphNodeMenuItem& left, const GraphNodeMenuItem& right)
		{
			return std::tie(left.Node->Category, left.Node->Kind) < std::tie(right.Node->Category, right.Node->Kind);
		});
		return items;
	}

}
