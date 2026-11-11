#include "TestsPCH.h"

#include "Engine/AssetPipeline/Graph/GraphDocument.h"

#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/Fuzz.h"
#include "Support/SceneTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// The graph document (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decision 4, stream G-A). The identifier, lookup and
// next-id cases are live (the contract implements them); the others are skipped until stream G-A implements the document,
// and the fuzz case until stream G-B's library and transpiler are integrated with it.

namespace Engine {

	namespace {

		// The door of the ADR's example: opens once when its Player enters its trigger, logging it.
		constexpr std::string_view DoorGraph = R"({
	"Format": "Graph",
	"Version": 1,
	"Name": "Door",
	"Fields": [
		{ "Name": "Player", "Kind": "Entity", "Default": null, "Element": "Number", "AssetType": "", "EnumValues": [], "Options": {} },
		{ "Name": "Speed", "Kind": "Number", "Default": 2, "Element": "Number", "AssetType": "", "EnumValues": [],
			"Options": { "Max": 10, "Min": 0, "Tooltip": "How fast the door opens" } }
	],
	"Variables": [
		{ "Name": "Opened", "Kind": "Bool", "Default": false, "Element": "Number", "AssetType": "", "EnumValues": [] }
	],
	"Nodes": [
		{ "Id": 1, "Kind": "Event.OnTriggerEnter", "Position": [0, 0], "Values": {} },
		{ "Id": 2, "Kind": "Compare.Equal", "Position": [240, 120], "Values": {} },
		{ "Id": 3, "Kind": "Self.Get.Player", "Position": [0, 160], "Values": {} },
		{ "Id": 4, "Kind": "Flow.Branch", "Position": [480, 0], "Values": {} },
		{ "Id": 5, "Kind": "Self.Set.Opened", "Position": [720, 0], "Values": { "Value": true } },
		{ "Id": 6, "Kind": "Call.Log.Info", "Position": [960, 0], "Values": { "Values": "Door opened" } }
	],
	"Links": [
		{ "From": { "Node": 1, "Pin": "Then" }, "To": { "Node": 4, "Pin": "In" } },
		{ "From": { "Node": 1, "Pin": "other" }, "To": { "Node": 2, "Pin": "A" } },
		{ "From": { "Node": 2, "Pin": "Result" }, "To": { "Node": 4, "Pin": "Condition" } },
		{ "From": { "Node": 3, "Pin": "Value" }, "To": { "Node": 2, "Pin": "B" } },
		{ "From": { "Node": 4, "Pin": "True" }, "To": { "Node": 5, "Pin": "In" } },
		{ "From": { "Node": 5, "Pin": "Then" }, "To": { "Node": 6, "Pin": "In" } }
	]
})";

		GraphDocument ReadDoor(const TypeRegistry& registry)
		{
			GraphLoadReport report;
			Result<GraphDocument> graph = GraphFromText(DoorGraph, registry, report, true);
			REQUIRE_MESSAGE(graph.has_value(), graph.error().ToString());
			CHECK(report.FileVersion == GraphFormatVersion);
			CHECK(report.Diagnostics.empty());
			return std::move(*graph);
		}

		// The pointers of a Validation error's issues, in order.
		std::vector<std::string> IssuePointers(const Status& status)
		{
			std::vector<std::string> pointers;
			if (status.has_value())
				return pointers;
			for (const ErrorIssue& issue : status.error().GetIssues())
				pointers.push_back(issue.JsonPointer);
			return pointers;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("GraphDocument: a graph identifier is a Luau name of at most 64 bytes, not a reserved word, that does not start with two underscores")
		{
			CHECK(IsGraphIdentifier("Door"));
			CHECK(IsGraphIdentifier("_speed2"));
			CHECK(IsGraphIdentifier(std::string(MaxGraphNameBytes, 'a')));
			CHECK_FALSE(IsGraphIdentifier(""));
			CHECK_FALSE(IsGraphIdentifier("2fast"));
			CHECK_FALSE(IsGraphIdentifier("__GraphLive"));
			CHECK_FALSE(IsGraphIdentifier("door-open"));
			CHECK_FALSE(IsGraphIdentifier("door.open"));
			CHECK_FALSE(IsGraphIdentifier(std::string(MaxGraphNameBytes + 1, 'a')));
			// Luau's reserved words cannot follow "self." or start "<Name> = Field..." in the transpiled text; a word that only
			// contains one, or differs in case, can.
			for (const std::string_view word : { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in", "local", "nil", "not",
					 "or", "repeat", "return", "then", "true", "until", "while" })
				CHECK_MESSAGE(!IsGraphIdentifier(word), word);
			CHECK(IsGraphIdentifier("End"));
			CHECK(IsGraphIdentifier("ending"));
			CHECK(IsGraphIdentifier("nil_count"));
			CHECK(IsGraphIdentifier("continue")); // a contextual keyword, a valid name
		}

		TEST_CASE("GraphDocument: FindGraphNode finds a node by id and the next id follows the highest")
		{
			GraphDocument graph;
			CHECK(GetNextGraphNodeId(graph) == 1);
			CHECK(FindGraphNode(graph, 1) == nullptr);
			graph.Nodes = { GraphNode{ .Id = 2, .Kind = "Flow.Branch" }, GraphNode{ .Id = 7, .Kind = "Self.Entity" } };
			REQUIRE(FindGraphNode(graph, 7) != nullptr);
			CHECK(FindGraphNode(graph, 7)->Kind == "Self.Entity");
			CHECK(FindGraphNode(graph, 2)->Kind == "Flow.Branch");
			CHECK(FindGraphNode(graph, 3) == nullptr);
			CHECK(GetNextGraphNodeId(graph) == 8);
			graph.Nodes.push_back(GraphNode{ .Id = MaxGraphNodeId, .Kind = "Self.Entity" });
			CHECK(GetNextGraphNodeId(graph) == 0);
		}

		TEST_CASE("GraphDocument: a graph file reads strictly and writes back byte-identical in its canonical form" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const GraphDocument door = ReadDoor(*registry);
			CHECK(door.Name == "Door");
			REQUIRE(door.Fields.size() == 2);
			CHECK(door.Fields[1].Name == "Speed");
			CHECK(door.Fields[1].Kind == GraphValueKind::Number);
			CHECK(door.Fields[1].Options.at("Max") == VariantValue(Json(10)));
			REQUIRE(door.Variables.size() == 1);
			CHECK(door.Variables[0].Kind == GraphValueKind::Bool);
			REQUIRE(door.Nodes.size() == 6);
			CHECK(door.Nodes[5].Values.at("Values") == VariantValue(Json("Door opened")));
			REQUIRE(door.Links.size() == 6);
			CHECK(door.Links[1].From == GraphPinRef{ .Node = 1, .Pin = "other" });
			CHECK(ValidateGraphStructure(door).has_value());

			// The canonical text reads back to the same document and writes again to the same bytes.
			const Result<std::string> text = GraphToText(door, *registry);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			GraphLoadReport report;
			const Result<GraphDocument> again = GraphFromText(*text, *registry, report, true);
			REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
			CHECK(*again == door);
			CHECK(GraphToText(*again, *registry).value_or(std::string()) == *text);

			// A newer file is refused naming both versions; an unknown member warns, or fails strictly.
			Json newer = JsonReader::Parse(*text).value_or(Json());
			newer["Version"] = GraphFormatVersion + 1;
			CHECK(GraphFromJson(newer, *registry, report).error().GetCode() == ErrorCode::UnsupportedVersion);
			Json unknown = JsonReader::Parse(*text).value_or(Json());
			unknown["Comment"] = "hand-written";
			GraphLoadReport warned;
			CHECK(GraphFromJson(unknown, *registry, warned).has_value());
			CHECK(warned.Diagnostics.size() == 1);
			CHECK_FALSE(GraphFromJson(unknown, *registry, warned, true).has_value());
		}

		TEST_CASE("GraphDocument: the structural rules refuse each violation at its JSON pointer" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const GraphDocument door = ReadDoor(*registry);

			GraphDocument graph = door;
			graph.Name = "__Door";
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Name" });

			graph = door;
			graph.Variables[0].Name = "Speed"; // a field has the name
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Variables/0/Name" });

			graph = door;
			graph.Fields[0].Name = "OnStart"; // reserved
			graph.Fields[1].Options["Step"] = VariantValue(Json("fast"));
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Fields/0/Name", "/Fields/1/Options/Step" });

			graph = door;
			graph.Variables[0].Name = "end";                                      // a Luau reserved word
			graph.Fields[0].Options["Tooltip"] = VariantValue(Json("Who opens")); // Field.Entity() takes no FieldOptions
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Fields/0/Options/Tooltip", "/Variables/0/Name" });

			graph = door;
			graph.Nodes[1].Id = 1; // a duplicate id
			graph.Nodes[2].Kind = "Self..Get";
			graph.Nodes[3].Position.x = MaxGraphPosition * 2.0f;
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Nodes/1/Id", "/Nodes/2/Kind", "/Nodes/3/Position" });

			graph = door;
			graph.Nodes[4].Values["bad pin"] = VariantValue(Json(1));
			graph.Nodes[5].Values["Values"] = VariantValue(Json::array({ 1, 2 })); // an array of 3 or 4 numbers only
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Nodes/4/Values/bad pin", "/Nodes/5/Values/Values" });

			graph = door;
			graph.Links[0].To.Node = 99;          // no such node
			graph.Links[3].From.Node = 2;         // a node linked to itself
			graph.Links.push_back(door.Links[5]); // repeated, and out of order
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Links/0", "/Links/3", "/Links/6" });

			// CanonicalizeGraph sorts every list and drops the repeated link, which then validates.
			graph = door;
			std::ranges::reverse(graph.Nodes);
			std::ranges::reverse(graph.Links);
			graph.Links.push_back(door.Links[0]);
			CanonicalizeGraph(graph);
			CHECK(graph == door);
		}

		TEST_CASE("GraphDocument: 1,000 seeded mutations of a graph file never crash the reader, the validation or the transpiler"
			* doctest::test_suite(Test::FuzzSuite) * doctest::skip(true))
		{
			// The seed corpus: the door's canonical text. A mutated text goes through GraphFromText (strict or not); a document it
			// accepts is valid structurally, writes back, and goes through ValidateGraph and, when that finds nothing, through
			// TranspileGraph and FindGraphAssetLiterals, so the library's and the transpiler's handling of every accepted document
			// is fuzzed with the reader's.
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			ScriptApiRegistry api;
			REQUIRE(RegisterBindings(api, *registry).has_value());
			const Result<Scope<GraphNodeLibrary>> library = GraphNodeLibrary::Create(api);
			REQUIRE_MESSAGE(library.has_value(), library.error().ToString());
			const Result<std::string> seedText = GraphToText(ReadDoor(*registry), *registry);
			REQUIRE_MESSAGE(seedText.has_value(), seedText.error().ToString());
			const Test::FuzzRun run = Test::RunFuzzCase(0x6A7A0F24, 1000, [&registry, &library, &seedText](Random& random, uint64_t /*index*/)
			{
				std::string mutated = *seedText;
				Test::MutateText(mutated, random);
				GraphLoadReport report;
				const Result<GraphDocument> graph = GraphFromText(mutated, *registry, report, random.NextBool());
				if (!graph.has_value())
					return false;
				CHECK(ValidateGraphStructure(*graph).has_value());
				CHECK(GraphToText(*graph, *registry).has_value());
				const std::vector<GraphIssue> issues = ValidateGraph(*graph, **library);
				static_cast<void>(FindGraphAssetLiterals(*graph, **library));
				const Result<GraphTranspileResult> transpiled = TranspileGraph(*graph, **library, "Assets/Door.graph");
				CHECK(transpiled.has_value() == issues.empty());
				return true;
			});
			// Some mutations keep a readable graph and most break it.
			CHECK(run.Counts.Accepted > 0);
			CHECK(run.Counts.Rejected > run.Counts.Accepted);
		}
	}

}
