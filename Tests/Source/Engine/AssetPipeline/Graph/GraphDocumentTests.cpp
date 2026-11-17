#include "TestsPCH.h"

#include "Engine/AssetPipeline/Graph/GraphDocument.h"

#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/RandomValueGenerator.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/Fuzz.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// The graph document (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decision 4, stream G-A). The identifier, lookup and
// next-id cases are the contract's; the document's are stream G-A's; the fuzz case runs G-B's library and transpiler over
// every document the reader accepts (W24.1's integration).

namespace Engine {

	namespace {

		// The door of the ADR's example: opens once when its Player enters its trigger, logging it. Version 2 (M39), without the
		// members version 2 added, which a reader leaves at their defaults.
		constexpr std::string_view DoorGraph = R"({
	"Format": "Graph",
	"Version": 2,
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

		// A graph named Probe holding the one field `field`.
		GraphDocument WithField(GraphField field)
		{
			GraphDocument graph;
			graph.Name = "Probe";
			graph.Fields = { std::move(field) };
			return graph;
		}

		// A graph named Probe holding the one variable `variable`.
		GraphDocument WithVariable(GraphVariable variable)
		{
			GraphDocument graph;
			graph.Name = "Probe";
			graph.Variables = { std::move(variable) };
			return graph;
		}

		VariantValue JsonValue(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE_MESSAGE(json.has_value(), json.error().ToString());
			return VariantValue(std::move(*json));
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

		TEST_CASE("GraphDocument: a graph file reads strictly and writes back byte-identical in its canonical form")
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

		TEST_CASE("GraphDocument: the structural rules refuse each violation at its JSON pointer")
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

		// The canonical text is a pure function of the document (JsonWriter's Pretty style, the header first, every field in
		// registry order, maps by key, floats in their shortest form), so the Debug and Release unit runs both match these bytes.
		TEST_CASE("GraphDocument: a graph's canonical text is the same bytes in every configuration")
		{
			constexpr std::string_view Expected = R"({
	"Format": "Graph",
	"Version": 2,
	"Name": "Counter",
	"RunsWhileWorldPaused": false,
	"Fields": [
		{
			"Name": "Step",
			"Kind": "Integer",
			"Default": 1,
			"Element": "Number",
			"AssetType": "",
			"EnumValues": [],
			"Options": {
				"Min": 1,
				"Tooltip": "Added each start"
			}
		}
	],
	"Variables": [
		{
			"Name": "Count",
			"Kind": "Integer",
			"Default": 0,
			"Element": "Number",
			"AssetType": "",
			"EnumValues": []
		}
	],
	"Methods": [],
	"Nodes": [
		{
			"Id": 1,
			"Kind": "Event.OnStart",
			"Position": [0, 0],
			"Values": {}
		},
		{
			"Id": 2,
			"Kind": "Call.Log.Info",
			"Position": [240.5, -16],
			"Values": {
				"Values": "Started"
			}
		}
	],
	"Links": [
		{
			"From": {
				"Node": 1,
				"Pin": "Then"
			},
			"To": {
				"Node": 2,
				"Pin": "In"
			}
		}
	]
}
)";
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			GraphDocument counter;
			counter.Name = "Counter";
			counter.Fields = { GraphField{ .Name = "Step", .Kind = GraphValueKind::Integer, .Default = VariantValue(Json(1)), .Options = { { "Tooltip", VariantValue(Json("Added each start")) }, { "Min", VariantValue(Json(1)) } } } };
			counter.Variables = { GraphVariable{ .Name = "Count", .Kind = GraphValueKind::Integer, .Default = VariantValue(Json(0)) } };
			counter.Nodes = { GraphNode{ .Id = 1, .Kind = "Event.OnStart" },
				GraphNode{ .Id = 2, .Kind = "Call.Log.Info", .Position = glm::vec2(240.5f, -16.0f), .Values = { { "Values", VariantValue(Json("Started")) } } } };
			counter.Links = { GraphLink{ .From = { .Node = 1, .Pin = "Then" }, .To = { .Node = 2, .Pin = "In" } } };
			REQUIRE(ValidateGraphStructure(counter).has_value());
			CHECK(GraphToText(counter, *registry).value_or(std::string()) == Expected);
			GraphLoadReport report;
			const Result<GraphDocument> read = GraphFromText(Expected, *registry, report, true);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(*read == counter);
		}

		// A member's Default, Element, AssetType, EnumValues and Options follow its kind, and a field's are what its Luau Field
		// constructor accepts (a default it would refuse fails the graph structurally, never at the import's extraction).
		TEST_CASE("GraphDocument: a member's default, element, asset type, choices and options follow its kind and its Field constructor")
		{
			const auto pointers = [](GraphDocument graph)
			{
				return IssuePointers(ValidateGraphStructure(graph));
			};
			using Pointers = std::vector<std::string>;

			// Every kind with its own default, and with a valid default of its spelling, is valid as a field and as a variable.
			const std::vector<std::pair<GraphValueKind, std::string_view>> valid = { { GraphValueKind::Number, "2.5" }, { GraphValueKind::Integer, "-7" },
				{ GraphValueKind::Bool, "true" }, { GraphValueKind::String, "\"Door\"" }, { GraphValueKind::Vector, "[1, 2, 3]" },
				{ GraphValueKind::Color, "[1, 0.5, 0, 1]" }, { GraphValueKind::Quat, "[0, 0, 0, 1]" }, { GraphValueKind::Entity, "null" },
				{ GraphValueKind::Array, "[]" } };
			for (const auto& [kind, spelling] : valid)
			{
				CAPTURE(std::string(spelling));
				CHECK(pointers(WithField(GraphField{ .Name = "Value", .Kind = kind, .Default = JsonValue(spelling) })).empty());
				CHECK(pointers(WithVariable(GraphVariable{ .Name = "Value", .Kind = kind, .Default = JsonValue(spelling) })).empty());
			}
			CHECK(pointers(WithField(GraphField{ .Name = "Clip", .Kind = GraphValueKind::Asset, .AssetType = "AudioClip" })).empty());
			CHECK(pointers(WithField(GraphField{ .Name = "Mode", .Kind = GraphValueKind::Enum, .Default = JsonValue("\"Slow\""), .EnumValues = { "Fast", "Slow" } })).empty());
			CHECK(pointers(WithField(GraphField{ .Name = "Clips", .Kind = GraphValueKind::Array, .Default = JsonValue("[]"), .Element = GraphValueKind::Asset, .AssetType = "AudioClip" }))
					.empty());
			CHECK(pointers(WithField(GraphField{ .Name = "Modes", .Kind = GraphValueKind::Array, .Default = JsonValue("[]"), .Element = GraphValueKind::Enum, .EnumValues = { "Fast" } }))
					.empty());

			// A default of another spelling.
			for (const auto& [kind, spelling] : std::vector<std::pair<GraphValueKind, std::string_view>>{ { GraphValueKind::Number, "\"fast\"" },
					 { GraphValueKind::Number, "1e300" }, { GraphValueKind::Integer, "2.5" }, { GraphValueKind::Integer, "3000000000" },
					 { GraphValueKind::Bool, "0" }, { GraphValueKind::String, "null" }, { GraphValueKind::Vector, "[1, 2]" }, { GraphValueKind::Color, "[1, 1, 1]" },
					 { GraphValueKind::Quat, "[0, 0, 0, 0]" }, { GraphValueKind::Entity, "\"0000000000000001\"" }, { GraphValueKind::Array, "[1]" } })
			{
				CAPTURE(std::string(spelling));
				CHECK(pointers(WithField(GraphField{ .Name = "Value", .Kind = kind, .Default = JsonValue(spelling) })) == Pointers{ "/Fields/0/Default" });
				CHECK(pointers(WithVariable(GraphVariable{ .Name = "Value", .Kind = kind, .Default = JsonValue(spelling) })) == Pointers{ "/Variables/0/Default" });
			}
			CHECK(pointers(WithField(GraphField{ .Name = "Text", .Kind = GraphValueKind::String, .Default = VariantValue(Json(std::string(MaxGraphStringBytes + 1, 'a'))) }))
				== Pointers{ "/Fields/0/Default" });

			// Field.Quat requires a unit default; a variable's quaternion is any nonzero one.
			CHECK(pointers(WithField(GraphField{ .Name = "Turn", .Kind = GraphValueKind::Quat, .Default = JsonValue("[0, 0, 0, 2]") })) == Pointers{ "/Fields/0/Default" });
			CHECK(pointers(WithVariable(GraphVariable{ .Name = "Turn", .Kind = GraphValueKind::Quat, .Default = JsonValue("[0, 0, 0, 2]") })).empty());

			// An element only for an Array, never an Array; an asset type only for an Asset; choices only for an Enum, valid and
			// unique, and an Enum's default one of them (not checked while it has none: that is the choices' issue alone).
			CHECK(pointers(WithField(GraphField{ .Name = "Nested", .Kind = GraphValueKind::Array, .Default = JsonValue("[]"), .Element = GraphValueKind::Array }))
				== Pointers{ "/Fields/0/Element" });
			CHECK(pointers(WithField(GraphField{ .Name = "Speed", .Default = JsonValue("0"), .Element = GraphValueKind::Bool })) == Pointers{ "/Fields/0/Element" });
			CHECK(pointers(WithField(GraphField{ .Name = "Clip", .Kind = GraphValueKind::Asset })) == Pointers{ "/Fields/0/AssetType" });
			CHECK(pointers(WithField(GraphField{ .Name = "Clip", .Kind = GraphValueKind::Asset, .AssetType = "None" })) == Pointers{ "/Fields/0/AssetType" });
			CHECK(pointers(WithField(GraphField{ .Name = "Clip", .Kind = GraphValueKind::Asset, .AssetType = "Sound" })) == Pointers{ "/Fields/0/AssetType" });
			CHECK(pointers(WithField(GraphField{ .Name = "Speed", .Default = JsonValue("0"), .AssetType = "Texture" })) == Pointers{ "/Fields/0/AssetType" });
			CHECK(pointers(WithField(GraphField{ .Name = "Speed", .Default = JsonValue("0"), .EnumValues = { "Fast" } })) == Pointers{ "/Fields/0/EnumValues" });
			CHECK(pointers(WithField(GraphField{ .Name = "Mode", .Kind = GraphValueKind::Enum, .Default = JsonValue("\"Fast\"") })) == Pointers{ "/Fields/0/EnumValues" });
			CHECK(pointers(WithField(GraphField{ .Name = "Mode", .Kind = GraphValueKind::Enum, .Default = JsonValue("\"Fast\""), .EnumValues = { "Fast", "Fast", "" } }))
				== Pointers{ "/Fields/0/EnumValues/1", "/Fields/0/EnumValues/2" });
			CHECK(pointers(WithField(GraphField{ .Name = "Mode", .Kind = GraphValueKind::Enum, .Default = JsonValue("\"Slow\""), .EnumValues = { "Fast" } }))
				== Pointers{ "/Fields/0/Default" });
			const Pointers tooMany = pointers(WithField(
				GraphField{ .Name = "Mode", .Kind = GraphValueKind::Enum, .Default = JsonValue("\"A\""), .EnumValues = std::vector<std::string>(MaxGraphEnumValues + 1, "A") }));
			REQUIRE_FALSE(tooMany.empty());
			CHECK(tooMany.front() == "/Fields/0/EnumValues");

			// Options: the Luau FieldOptions of a kind whose constructor takes them, bounds only for Number, Integer and Vector,
			// Min <= Max, a positive Step, a short Tooltip; a field's default within its bounds as its reflected type compares it.
			const auto withOptions = [](GraphValueKind kind, std::string_view defaultValue, std::map<std::string, VariantValue> options)
			{
				return WithField(GraphField{ .Name = "Value", .Kind = kind, .Default = JsonValue(defaultValue), .Options = std::move(options) });
			};
			CHECK(pointers(withOptions(GraphValueKind::Number, "2", { { "Min", JsonValue("0") }, { "Max", JsonValue("10") }, { "Step", JsonValue("0.5") }, { "Tooltip", JsonValue("\"How fast\"") } }))
					.empty());
			CHECK(pointers(withOptions(GraphValueKind::Vector, "[1, 2, 3]", { { "Min", JsonValue("0") }, { "Max", JsonValue("3") } })).empty());
			CHECK(pointers(withOptions(GraphValueKind::Bool, "false", { { "Tooltip", JsonValue("\"Open\"") } })).empty());
			CHECK(pointers(withOptions(GraphValueKind::Number, "2", { { "Step", JsonValue("0") } })) == Pointers{ "/Fields/0/Options/Step" });
			CHECK(pointers(withOptions(GraphValueKind::Number, "2", { { "Max", JsonValue("1") }, { "Min", JsonValue("5") } })) == Pointers{ "/Fields/0/Options/Max" });
			CHECK(pointers(withOptions(GraphValueKind::Bool, "false", { { "Min", JsonValue("0") } })) == Pointers{ "/Fields/0/Options/Min" });
			CHECK(pointers(withOptions(GraphValueKind::Number, "2", { { "Label", JsonValue("\"Speed\"") } })) == Pointers{ "/Fields/0/Options/Label" });
			CHECK(pointers(withOptions(GraphValueKind::Number, "2", { { "Tooltip", VariantValue(Json(std::string(MaxGraphTooltipBytes + 1, 't'))) } }))
				== Pointers{ "/Fields/0/Options/Tooltip" });
			CHECK(pointers(withOptions(GraphValueKind::Number, "20", { { "Min", JsonValue("0") }, { "Max", JsonValue("10") } })) == Pointers{ "/Fields/0/Default" });
			CHECK(pointers(withOptions(GraphValueKind::Integer, "-1", { { "Min", JsonValue("0") } })) == Pointers{ "/Fields/0/Default" });
			CHECK(pointers(withOptions(GraphValueKind::Vector, "[1, 4, 3]", { { "Max", JsonValue("3") } })) == Pointers{ "/Fields/0/Default" });
			// 0.1 is no float: its float lies just above the double 0.1, and both sides are compared as floats, so it is within.
			CHECK(pointers(withOptions(GraphValueKind::Number, "0.1", { { "Max", JsonValue("0.1") } })).empty());
		}

		// Decision 7's one text (W24.1's review): a .graph file holds a number as JsonWriter writes it back, a fractional one as
		// the float it rounds to (-0 as 0). A number with more digits than its float keeps would transpile at double precision
		// until a canonical re-save (project.upgrade, any graph edit) rounded it, changing the behaviour; the reader refuses it at
		// its pointer, as it refuses another non-canonical file, naming the float to write.
		TEST_CASE("GraphDocument: a number that the writer would not write back as itself is refused at its pointer, naming the float to write")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			// A graph whose nodes hold `literal` as a Number and a Vector component, whose variables' Defaults hold `variable`
			// likewise, and whose field's Max is `maximum` (its Default the least float, within any Max).
			const auto document = [](std::string_view literal, std::string_view variable, std::string_view maximum)
			{
				return std::format(R"({{"Format": "Graph", "Version": 1, "Name": "Probe",
					"Fields": [{{"Name": "Rate", "Kind": "Number", "Default": -3.4e+38, "Element": "Number", "AssetType": "", "EnumValues": [], "Options": {{"Max": {}}}}}],
					"Variables": [{{"Name": "Scale", "Kind": "Number", "Default": {}, "Element": "Number", "AssetType": "", "EnumValues": []}},
						{{"Name": "Size", "Kind": "Vector", "Default": [1, {}, 0], "Element": "Number", "AssetType": "", "EnumValues": []}}],
					"Nodes": [{{"Id": 1, "Kind": "Number.Multiply", "Position": [0, 0], "Values": {{"B": {}}}}},
						{{"Id": 2, "Kind": "Self.Set.Size", "Position": [0, 0], "Values": {{"Value": [{}, 0, 0]}}}}],
					"Links": []}})",
					maximum, variable, variable, literal, literal);
			};
			const auto read = [&registry](const std::string& text)
			{
				GraphLoadReport report;
				return GraphFromText(text, *registry, report, true);
			};
			// The pointers of a refused read's issues; none for a read that succeeds.
			const auto refusedAt = [&read](const std::string& text)
			{
				const Result<GraphDocument> graph = read(text);
				return graph.has_value() ? std::vector<std::string>() : IssuePointers(std::unexpected(graph.error()));
			};

			// Numbers the writer writes back as themselves: integers, and the shortest spellings of floats.
			for (const std::string_view number : { "0.3", "2.5", "-0.75", "1e-05", "0.70710677", "3.4e+38", "16777217", "-2", "0" })
			{
				CAPTURE(std::string(number));
				const Result<GraphDocument> graph = read(document(number, number, number));
				REQUIRE_MESSAGE(graph.has_value(), graph.error().ToString());
				const Result<std::string> text = GraphToText(*graph, *registry);
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				const Result<GraphDocument> again = read(*text);
				REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
				CHECK(*again == *graph);
			}

			// 0.30000001 and 1e-50 have more digits than their floats keep (the writer writes 0.3 and 0), and -0.0 is written as 0:
			// each is refused at every place it stands, the message naming the float to write.
			for (const auto& [number, written] : { std::pair<std::string_view, std::string_view>{ "0.30000001", "write 0.3" },
					 std::pair<std::string_view, std::string_view>{ "1e-50", "write 0" }, std::pair<std::string_view, std::string_view>{ "-0.0", "write 0" } })
			{
				CAPTURE(std::string(number));
				const Result<GraphDocument> refused = read(document(number, number, number));
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::Validation);
				CHECK(IssuePointers(std::unexpected(refused.error()))
					== std::vector<std::string>{ "/Fields/0/Options/Max", "/Variables/0/Default", "/Variables/1/Default", "/Nodes/0/Values/B", "/Nodes/1/Values/Value" });
				for (const ErrorIssue& issue : refused.error().GetIssues())
					CHECK_MESSAGE(issue.Message.find(written) != std::string::npos, issue.Message);
				// Alone in one place it is refused there.
				CHECK(refusedAt(document("1", "1", number)) == std::vector<std::string>{ "/Fields/0/Options/Max" });
				CHECK(refusedAt(document(number, "1", "1")) == std::vector<std::string>{ "/Nodes/0/Values/B", "/Nodes/1/Values/Value" });
			}
		}

		TEST_CASE("GraphDocument: the limits bound every count and size a graph file controls")
		{
			const auto pointers = [](const GraphDocument& graph)
			{
				return IssuePointers(ValidateGraphStructure(graph));
			};
			using Pointers = std::vector<std::string>;

			GraphDocument graph;
			graph.Name = std::string(MaxGraphNameBytes + 1, 'N');
			CHECK(pointers(graph) == Pointers{ "/Name" });

			graph.Name = "Probe";
			for (size_t index = 0; index <= MaxGraphFields; ++index)
				graph.Fields.push_back(GraphField{ .Name = std::format("f{:04}", index), .Default = VariantValue(Json(0)) });
			CHECK(pointers(graph) == Pointers{ "/Fields" });
			graph.Fields.clear();
			for (size_t index = 0; index <= MaxGraphVariables; ++index)
				graph.Variables.push_back(GraphVariable{ .Name = std::format("v{:04}", index), .Default = VariantValue(Json(0)) });
			CHECK(pointers(graph) == Pointers{ "/Variables" });
			graph.Variables.clear();

			for (uint32_t id = 1; id <= MaxGraphNodes + 1; ++id)
				graph.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Flow.Sequence" });
			CHECK(pointers(graph) == Pointers{ "/Nodes" });
			graph.Nodes.resize(2);
			CHECK(pointers(graph).empty());

			for (size_t index = 0; index <= MaxGraphLinks; ++index)
				graph.Links.push_back(GraphLink{ .From = { 1, std::format("p{:05}", index) }, .To = { 2, "In" } });
			CHECK(pointers(graph) == Pointers{ "/Links" });
			graph.Links.clear();

			graph.Nodes[0].Kind = std::string(MaxGraphKindBytes + 1, 'K');
			for (size_t index = 0; index <= MaxGraphNodeValues; ++index)
				graph.Nodes[1].Values.emplace(std::format("p{:03}", index), VariantValue(Json(1)));
			CHECK(pointers(graph) == Pointers{ "/Nodes/0/Kind", "/Nodes/1/Values" });
			graph.Nodes[0].Kind = "Flow.Sequence";
			graph.Nodes[1].Values = { { "Text", VariantValue(Json(std::string(MaxGraphStringBytes + 1, 's'))) } };
			CHECK(pointers(graph) == Pointers{ "/Nodes/1/Values/Text" });
			graph.Nodes[1].Values = { { "Text", VariantValue(Json(std::string(MaxGraphStringBytes, 's'))) } };
			CHECK(pointers(graph).empty());
		}

		// A link names its nodes by id, so a node list whose ids are not valid and in order leaves the links' nodes unchecked:
		// the ids' issues stand for them. A literal is one of the file comment's shapes, and a link joins named pins.
		TEST_CASE("GraphDocument: node ids are valid, unique and in order before links are checked against them, and literals and pins keep their shapes")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const GraphDocument door = ReadDoor(*registry);

			GraphDocument graph = door;
			std::swap(graph.Nodes[0], graph.Nodes[1]); // ids 2, 1: out of order
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Nodes/1/Id" });
			graph = door;
			graph.Nodes[0].Id = 0;
			graph.Links[0].To.Node = 99; // unchecked while an id is invalid
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Nodes/0/Id" });
			graph.Nodes[0].Id = 1;
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Links/0" });

			graph = door;
			graph.Nodes[0].Values = { { "a", JsonValue("null") }, { "b", JsonValue("false") }, { "c", JsonValue("-2.5") }, { "d", JsonValue("[1, 2, 3]") },
				{ "e", JsonValue("[0, 0, 0, 1]") } };
			CHECK(ValidateGraphStructure(graph).has_value());
			graph.Nodes[0].Values = { { "a", JsonValue("{}") }, { "b", JsonValue("[1, \"x\", 3]") }, { "c", JsonValue("[1, 2, 3, 4, 5]") }, { "d", JsonValue("1e300") },
				{ "e", JsonValue("[[1, 2, 3]]") } };
			CHECK(IssuePointers(ValidateGraphStructure(graph))
				== std::vector<std::string>{ "/Nodes/0/Values/a", "/Nodes/0/Values/b", "/Nodes/0/Values/c", "/Nodes/0/Values/d", "/Nodes/0/Values/e" });
			// A pin key is escaped in its pointer (RFC 6901).
			graph.Nodes[0].Values = { { "a/b", JsonValue("1") } };
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Nodes/0/Values/a~1b" });

			graph = door;
			graph.Links[0].To.Pin = "in put";
			graph.Links[1].From.Pin = "end";
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Links/0", "/Links/1" });
		}

		// The registry reads the Graph struct's default (empty) object, as every reflected struct's, while a document needs its
		// Name; the header names the format and a version from 1.
		TEST_CASE("GraphDocument: a graph document needs its header and a Name, while the registry reads the empty default graph")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const StructInfo* type = registry->FindStruct("Graph");
			REQUIRE(type != nullptr);
			GraphDocument empty;
			CHECK(type->FromJson(&empty, JsonReader(type->MakeDefaultJson()), ReadContext{}).has_value());

			GraphLoadReport report;
			const Result<GraphDocument> nameless = GraphFromText(R"({"Format": "Graph", "Version": 1})", *registry, report, true);
			REQUIRE_FALSE(nameless.has_value());
			CHECK(nameless.error().GetCode() == ErrorCode::Validation);
			CHECK(IssuePointers(std::unexpected(nameless.error())) == std::vector<std::string>{ "/Name" });
			const Result<GraphDocument> sparse = GraphFromText(R"({"Format": "Graph", "Version": 1, "Name": "Sparse"})", *registry, report, true);
			REQUIRE_MESSAGE(sparse.has_value(), sparse.error().ToString());
			CHECK(sparse->Name == "Sparse");
			CHECK(sparse->Nodes.empty());

			for (const std::string_view text : { R"({"Format": "Scene", "Version": 1, "Name": "Door"})", R"({"Format": "Graph", "Version": 0, "Name": "Door"})",
					 R"({"Format": "Graph", "Name": "Door"})", R"({"Format": "Graph", "Version": 1, "Name": "Door", "Nodes": [{"Id": -1}]})",
					 R"({"Format": "Graph", "Version": 1, "Name": "Door", "Fields": [{"Name": "Speed", "Kind": "Float"}]})" })
			{
				CAPTURE(std::string(text));
				const Result<GraphDocument> refused = GraphFromText(text, *registry, report);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::Validation);
			}
			CHECK(GraphFromText("{\"Format\": \"Graph\",", *registry, report).error().GetCode() == ErrorCode::Parse);
		}

		// The Graph struct's Generate hook (the registry suite's random values) gives graphs that pass every structural rule
		// and read back from their canonical text.
		TEST_CASE("GraphDocument: the registry's random graphs pass the structural rules and read back from their canonical text")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const StructInfo* type = registry->FindStruct<GraphDocument>();
			REQUIRE(type != nullptr);
			RandomValueGenerator generator(*registry, 0x6a7a0001);
			size_t members = 0;
			size_t links = 0;
			for (int iteration = 0; iteration < 200; ++iteration)
			{
				CAPTURE(iteration);
				GraphDocument graph;
				generator.Randomize(*type, &graph);
				const Status valid = ValidateGraphStructure(graph);
				REQUIRE_MESSAGE(valid.has_value(), valid.error().ToString());
				const Result<std::string> text = GraphToText(graph, *registry);
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				GraphLoadReport report;
				const Result<GraphDocument> again = GraphFromText(*text, *registry, report, true);
				REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
				CHECK(GraphToText(*again, *registry).value_or(std::string()) == *text);
				members += graph.Fields.size() + graph.Variables.size();
				links += graph.Links.size();
			}
			// The random graphs are not empty ones.
			CHECK(members > 0);
			CHECK(links > 0);
		}

		// M39 (Docs/Decisions/0056-m39-graphs-behaviours.md decision 1): graph format 2 reads every version 1 file as version 2
		// with no methods and the opt-in false, the members version 2 added being unknown in a version 1 file and its Any kind
		// refused; project.upgrade writes v1.upgraded.graph's bytes (ProjectMethodsTests.cpp).
		TEST_CASE("GraphDocument: a version 1 graph reads as version 2 without methods")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const Result<std::string> v1 = Test::ReadTestDataText("Formats/Graph/v1.graph");
			const Result<std::string> upgraded = Test::ReadTestDataText("Formats/Graph/v1.upgraded.graph");
			REQUIRE_MESSAGE(v1.has_value(), v1.error().ToString());
			REQUIRE_MESSAGE(upgraded.has_value(), upgraded.error().ToString());
			GraphLoadReport report;
			const Result<GraphDocument> graph = GraphFromText(*v1, *registry, report, true);
			REQUIRE_MESSAGE(graph.has_value(), graph.error().ToString());
			CHECK(report.FileVersion == 1);
			CHECK(report.Diagnostics.empty());
			CHECK(graph->Methods.empty());
			CHECK_FALSE(graph->RunsWhileWorldPaused);
			CHECK(GraphToText(*graph, *registry).value_or(std::string()) == *upgraded);
			// The upgraded text reads as the same document, at the current version.
			const Result<GraphDocument> again = GraphFromText(*upgraded, *registry, report, true);
			REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
			CHECK(report.FileVersion == GraphFormatVersion);
			CHECK(*again == *graph);

			// In a version 1 file the members version 2 added are unknown: warned of and dropped, or refused strictly.
			Json withMembers = JsonReader::Parse(*v1).value_or(Json());
			withMembers["RunsWhileWorldPaused"] = true;
			withMembers["Methods"] = Json::array({ Json{ { "Name", "Pull" }, { "Parameters", Json::array() }, { "Results", Json::array() } } });
			GraphLoadReport warned;
			const Result<GraphDocument> dropped = GraphFromJson(withMembers, *registry, warned);
			REQUIRE_MESSAGE(dropped.has_value(), dropped.error().ToString());
			CHECK(dropped->Methods.empty());
			CHECK_FALSE(dropped->RunsWhileWorldPaused);
			REQUIRE(warned.Diagnostics.size() == 2);
			CHECK(warned.Diagnostics[0].JsonPointer == "/RunsWhileWorldPaused");
			CHECK(warned.Diagnostics[1].JsonPointer == "/Methods");
			const Result<GraphDocument> strict = GraphFromJson(withMembers, *registry, warned, true);
			REQUIRE_FALSE(strict.has_value());
			CHECK(IssuePointers(std::unexpected(strict.error())) == std::vector<std::string>{ "/RunsWhileWorldPaused", "/Methods" });

			// A version 1 variable of kind Any is refused at its Kind; the same variable reads at version 2.
			Json withAny = JsonReader::Parse(*v1).value_or(Json());
			withAny["Variables"][0]["Kind"] = "Any";
			withAny["Variables"][0]["Default"] = nullptr;
			const Result<GraphDocument> refused = GraphFromJson(withAny, *registry, warned);
			REQUIRE_FALSE(refused.has_value());
			CHECK(IssuePointers(std::unexpected(refused.error())) == std::vector<std::string>{ "/Variables/0/Kind" });
			withAny["Version"] = GraphFormatVersion;
			const Result<GraphDocument> current = GraphFromJson(withAny, *registry, warned, true);
			REQUIRE_MESSAGE(current.has_value(), current.error().ToString());
			CHECK(current->Variables[0].Kind == GraphValueKind::Any);
		}

		// M39 (decision 1 and departures 2 and 4): a method's name, parameters and result follow their rules, each violation
		// located; the Any kind is a variable's, a parameter's or a result's, never a field's or an element's.
		TEST_CASE("GraphDocument: a method's declaration follows the method rules, each violation at its pointer")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const GraphDocument door = ReadDoor(*registry);
			const auto method = [](std::string name, std::vector<GraphParameter> parameters, std::vector<GraphResult> results)
			{
				return GraphMethod{ .Name = std::move(name), .Parameters = std::move(parameters), .Results = std::move(results) };
			};
			const auto parameter = [](std::string name, GraphValueKind kind, std::string assetType = {})
			{
				return GraphParameter{ .Name = std::move(name), .Kind = kind, .AssetType = std::move(assetType) };
			};
			const auto result = [](GraphValueKind kind, std::string assetType = {})
			{
				return GraphResult{ .Kind = kind, .AssetType = std::move(assetType) };
			};

			// A valid declaration: parameters of several kinds, an Asset of any type and of one type, a result.
			GraphDocument graph = door;
			graph.Methods = { method("Blocks", { parameter("hit", GraphValueKind::Any), parameter("clip", GraphValueKind::Asset), parameter("icon", GraphValueKind::Asset, "Texture") },
								  { result(GraphValueKind::Bool) }),
				method("Pull", {}, {}) };
			CHECK(ValidateGraphStructure(graph).has_value());
			CHECK(IsGraphMethodName("Pull"));
			CHECK_FALSE(IsGraphMethodName("RunsWhileWorldPaused"));
			CHECK_FALSE(IsGraphMethodName("OnUiClick"));
			CHECK(IsGraphMethodValueKind(GraphValueKind::Any));
			CHECK_FALSE(IsGraphMethodValueKind(GraphValueKind::Enum));
			CHECK_FALSE(IsGraphMethodValueKind(GraphValueKind::Array));

			// Names: not an identifier, reserved, the class key, a field's name, out of order.
			graph.Methods = { method("2nd", {}, {}) };
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Methods/0/Name" });
			graph.Methods = { method("OnStart", {}, {}), method("RunsWhileWorldPaused", {}, {}) };
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Methods/0/Name", "/Methods/1/Name" });
			graph.Methods = { method("Speed", {}, {}) }; // a field's name
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Methods/0/Name" });
			graph.Methods = { method("Pull", {}, {}), method("Open", {}, {}) };
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Methods/1/Name" });

			// Parameters: at most four, named neither self nor Then, unique, of a method's kinds, an AssetType for an Asset only.
			graph.Methods = { method("Pull",
				{ parameter("a", GraphValueKind::Number), parameter("b", GraphValueKind::Number), parameter("c", GraphValueKind::Number), parameter("d", GraphValueKind::Number),
					parameter("e", GraphValueKind::Number) },
				{}) };
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Methods/0/Parameters" });
			graph.Methods = { method("Pull", { parameter("self", GraphValueKind::Any), parameter("Then", GraphValueKind::Any), parameter("x", GraphValueKind::Any) }, {}) };
			graph.Methods[0].Parameters.push_back(parameter("x", GraphValueKind::Any));
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Methods/0/Parameters/0/Name", "/Methods/0/Parameters/1/Name", "/Methods/0/Parameters/3/Name" });
			graph.Methods = { method("Pull", { parameter("mode", GraphValueKind::Enum), parameter("list", GraphValueKind::Array), parameter("speed", GraphValueKind::Number, "Texture"), parameter("clip", GraphValueKind::Asset, "Nope") },
				{}) };
			CHECK(IssuePointers(ValidateGraphStructure(graph))
				== std::vector<std::string>{ "/Methods/0/Parameters/0/Kind", "/Methods/0/Parameters/1/Kind", "/Methods/0/Parameters/2/AssetType", "/Methods/0/Parameters/3/AssetType" });

			// Results: at most one (departure 2: a second is refused at its pointer), of a method's kinds.
			graph.Methods = { method("Pull", {}, { result(GraphValueKind::Bool), result(GraphValueKind::Number) }) };
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Methods/0/Results/1" });
			graph.Methods = { method("Pull", {}, { result(GraphValueKind::Array) }) };
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Methods/0/Results/0/Kind" });

			// At most MaxGraphMethods methods.
			graph.Methods.clear();
			for (size_t index = 0; index <= MaxGraphMethods; ++index)
				graph.Methods.push_back(method(std::format("M{:03}", index), {}, {}));
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Methods" });

			// Any: never a field's kind or an element; a variable's Default is null.
			graph = door;
			graph.Fields[1].Kind = GraphValueKind::Any;
			graph.Variables[0].Kind = GraphValueKind::Array;
			graph.Variables[0].Element = GraphValueKind::Any;
			graph.Variables[0].Default = VariantValue(Json::array());
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Fields/1/Kind", "/Variables/0/Element" });
			graph = door;
			graph.Variables[0].Kind = GraphValueKind::Any;
			CHECK(IssuePointers(ValidateGraphStructure(graph)) == std::vector<std::string>{ "/Variables/0/Default" });
			graph.Variables[0].Default = VariantValue(Json(nullptr));
			CHECK(ValidateGraphStructure(graph).has_value());

			// A graph's methods and opt-in write back through the canonical text.
			graph.RunsWhileWorldPaused = true;
			graph.Methods = { method("Pull", { parameter("by", GraphValueKind::Entity) }, { result(GraphValueKind::Any) }) };
			const Result<std::string> text = GraphToText(graph, *registry);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			GraphLoadReport report;
			const Result<GraphDocument> read = GraphFromText(*text, *registry, report, true);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(*read == graph);
		}

		TEST_CASE("GraphDocument: 1,000 seeded mutations of a graph file never crash the reader, the validation or the transpiler"
			* doctest::test_suite(Test::FuzzSuite))
		{
			// The seed corpus: the door's canonical text and (M39, Docs/Decisions/0056-m39-graphs-behaviours.md) the Rune fixture's,
			// a graph with methods, returns through a merge, a callback's return and Script.Get, taken in turn. A mutated text goes
			// through GraphFromText (strict or not); a document it accepts is valid structurally, writes back, and goes through
			// ValidateGraph and, when that finds nothing, through TranspileGraph and FindGraphAssetLiterals, so the library's and
			// the transpiler's handling of every accepted document is fuzzed with the reader's.
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			ScriptApiRegistry api;
			REQUIRE(RegisterBindings(api, *registry).has_value());
			const Result<Scope<GraphNodeLibrary>> library = GraphNodeLibrary::Create(api);
			REQUIRE_MESSAGE(library.has_value(), library.error().ToString());
			const Result<std::string> doorText = GraphToText(ReadDoor(*registry), *registry);
			REQUIRE_MESSAGE(doorText.has_value(), doorText.error().ToString());
			const Result<std::string> runeText = Test::ReadTestDataText("Graphs/Rune.graph");
			REQUIRE_MESSAGE(runeText.has_value(), runeText.error().ToString());
			const std::array<const std::string*, 2> seeds = { &*doorText, &*runeText };
			const Test::FuzzRun run = Test::RunFuzzCase(0x6A7A0F24, 1000, [&registry, &library, &seeds](Random& random, uint64_t index)
			{
				std::string mutated = *seeds[index % seeds.size()];
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
