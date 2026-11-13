#include "TestsPCH.h"

#include "EditorCore/Graph/GraphNodeMenu.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

// The graph canvas's node menu (M24, W24.2's stream A; Docs/Decisions/0035-m24-contract.md decision 15): the kinds graph.nodes
// lists, searched and by category, and for a dropped link only the kinds it can join.

namespace Engine {

	namespace {

		GraphPinDescription MakePin(std::string name, GraphPinDirection direction, GraphPinType type, bool optional = false)
		{
			return GraphPinDescription{ .Name = std::move(name), .Direction = direction, .Type = type, .TypeName = {}, .Optional = optional, .Default = {} };
		}

		GraphNodeDescription MakeNode(std::string kind, std::string title, GraphNodeCategory category, std::vector<GraphPinDescription> pins)
		{
			return GraphNodeDescription{ .Kind = std::move(kind), .Title = std::move(title), .Category = category, .Description = {}, .Pure = false, .Pins = std::move(pins) };
		}

		// A small library, sorted by kind as graph.nodes lists it.
		std::vector<GraphNodeDescription> MakeLibrary()
		{
			using enum GraphPinDirection;
			return {
				MakeNode("Call.Assets.Load", "Load Asset", GraphNodeCategory::Call, { MakePin("reference", Input, GraphPinType::String), MakePin("Result", Output, GraphPinType::Asset, true) }),
				MakeNode("Call.Log.Info", "Log Info", GraphNodeCategory::Call,
					{ MakePin("In", Input, GraphPinType::Exec), MakePin("Values", Input, GraphPinType::Any, true), MakePin("Then", Output, GraphPinType::Exec) }),
				MakeNode("Event.OnStart", "On Start", GraphNodeCategory::Event, { MakePin("Then", Output, GraphPinType::Exec) }),
				MakeNode("Flow.Branch", "Branch", GraphNodeCategory::Flow,
					{ MakePin("In", Input, GraphPinType::Exec), MakePin("Condition", Input, GraphPinType::Bool), MakePin("True", Output, GraphPinType::Exec),
						MakePin("False", Output, GraphPinType::Exec) }),
				MakeNode("Number.Add", "Add", GraphNodeCategory::Operator, { MakePin("A", Input, GraphPinType::Number), MakePin("B", Input, GraphPinType::Number), MakePin("Result", Output, GraphPinType::Number) }),
			};
		}

		std::vector<std::string> Kinds(const std::vector<GraphNodeMenuItem>& items)
		{
			std::vector<std::string> kinds;
			for (const GraphNodeMenuItem& item : items)
				kinds.push_back(item.Node->Kind);
			return kinds;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("GraphNodeMenu: the menu lists kinds by category then kind, and a search matches every word in the kind or the title")
		{
			const std::vector<GraphNodeDescription> library = MakeLibrary();
			CHECK(Kinds(FilterGraphNodeMenu(library, "", nullptr)) == std::vector<std::string>{ "Event.OnStart", "Flow.Branch", "Number.Add", "Call.Assets.Load", "Call.Log.Info" });
			CHECK(Kinds(FilterGraphNodeMenu(library, "log", nullptr)) == std::vector<std::string>{ "Call.Log.Info" });
			// Case is ignored, a word may match the title ("Load Asset") or the kind, and every word must match.
			CHECK(Kinds(FilterGraphNodeMenu(library, "ASSET load", nullptr)) == std::vector<std::string>{ "Call.Assets.Load" });
			CHECK(Kinds(FilterGraphNodeMenu(library, "call  info", nullptr)) == std::vector<std::string>{ "Call.Log.Info" });
			CHECK(FilterGraphNodeMenu(library, "branch nope", nullptr).empty());
			for (const GraphNodeMenuItem& item : FilterGraphNodeMenu(library, "", nullptr))
				CHECK(item.Pin.empty());

			CHECK(GetGraphNodeCategoryLabel(GraphNodeCategory::Event) == "Events");
			CHECK(GetGraphNodeCategoryLabel(GraphNodeCategory::Constant) == "Constants");
			CHECK(GetGraphNodeCategoryLabel(static_cast<GraphNodeCategory>(200)).empty());
		}

		TEST_CASE("GraphNodeMenu: a dropped link lists only the kinds with a pin it joins, naming that pin")
		{
			const std::vector<GraphNodeDescription> library = MakeLibrary();
			// From an exec output: the statements, at their exec input.
			const GraphPinDescription then = MakePin("Then", GraphPinDirection::Output, GraphPinType::Exec);
			const std::vector<GraphNodeMenuItem> statements = FilterGraphNodeMenu(library, "", &then);
			CHECK(Kinds(statements) == std::vector<std::string>{ "Flow.Branch", "Call.Log.Info" });
			for (const GraphNodeMenuItem& item : statements)
				CHECK(item.Pin == "In");

			// From a number output: its first compatible input of each kind (an Any input takes it too).
			const GraphPinDescription number = MakePin("Result", GraphPinDirection::Output, GraphPinType::Number);
			const std::vector<GraphNodeMenuItem> numbers = FilterGraphNodeMenu(library, "", &number);
			REQUIRE(Kinds(numbers) == std::vector<std::string>{ "Number.Add", "Call.Log.Info" });
			CHECK(numbers[0].Pin == "A");
			CHECK(numbers[1].Pin == "Values");

			// An optional output feeds only optional inputs, never Number.Add's required A or B.
			const GraphPinDescription maybe = MakePin("Result", GraphPinDirection::Output, GraphPinType::Asset, true);
			CHECK(Kinds(FilterGraphNodeMenu(library, "", &maybe)) == std::vector<std::string>{ "Call.Log.Info" });

			// From a Bool input: the kinds with an output that feeds it; none here gives a Bool.
			const GraphPinDescription condition = MakePin("Condition", GraphPinDirection::Input, GraphPinType::Bool);
			CHECK(FilterGraphNodeMenu(library, "", &condition).empty());
			// From a Number input: Number.Add's Result, and the search still applies.
			const GraphPinDescription input = MakePin("B", GraphPinDirection::Input, GraphPinType::Number);
			const std::vector<GraphNodeMenuItem> sources = FilterGraphNodeMenu(library, "add", &input);
			REQUIRE(sources.size() == 1);
			CHECK(sources[0].Node->Kind == "Number.Add");
			CHECK(sources[0].Pin == "Result");
		}
	}

}
