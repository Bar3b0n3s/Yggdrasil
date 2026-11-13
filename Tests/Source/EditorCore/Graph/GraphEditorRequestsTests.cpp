#include "TestsPCH.h"

#include "EditorCore/Graph/GraphEditorRequests.h"

#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AutomationTestClient.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

// The requests the graph canvas submits (M24, W24.2's stream A; Docs/Decisions/0035-m24-contract.md decision 15): each action's
// method with its params, run here through automation's own client, so the canvas edits a graph exactly as an agent does and
// the composed actions are one undo step each.

namespace Engine {

	namespace {

		constexpr std::string_view DoorPath = "Assets/Scripts/Door.graph";

		// Runs `request` (the action `action`'s) through the automation client and returns its result.
		Json Run(Test::AutomationFixture& setup, const Result<GraphEditorRequest>& request, GraphEditorAction action)
		{
			REQUIRE_MESSAGE(request.has_value(), request.error().ToString());
			CHECK(request->Action == action);
			CHECK(request->Method == GetGraphEditorActionMethod(action));
			Result<Json> result = setup.Call(request->Method, request->Params);
			INFO(request->Params.dump());
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

		Json ReadDocument(Test::AutomationFixture& setup)
		{
			Result<Json> read = setup.Call("graph.read", Json{ { "graph", std::string(DoorPath) } });
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			return (*read)["document"];
		}

		std::string ReadDoorText(Test::AutomationFixture& setup)
		{
			const VfsPath file = VfsPath::Create("project", DoorPath).value_or(VfsPath());
			return setup.GetEditor().GetVfs().ReadText(file).value_or(std::string());
		}

		uint32_t ReadNode(const Json& result)
		{
			return JsonReader(result["node"]).ReadUInt32().value_or(0);
		}

		GraphAddNodeParams AddParams(std::string_view kind, glm::vec2 position, std::map<std::string, VariantValue> values = {})
		{
			return GraphAddNodeParams{ .Graph = std::string(DoorPath), .Kind = std::string(kind), .Position = position, .Values = std::move(values) };
		}

		GraphPinRef PinOf(uint32_t node, std::string_view pin)
		{
			return GraphPinRef{ .Node = node, .Pin = std::string(pin) };
		}

		Json LinkJson(uint32_t from, std::string_view fromPin, uint32_t to, std::string_view toPin)
		{
			return Json{ { "From", Json{ { "Node", from }, { "Pin", std::string(fromPin) } } }, { "To", Json{ { "Node", to }, { "Pin", std::string(toPin) } } } };
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("GraphEditorRequests: each single-method action submits its method with that method's params")
		{
			Test::AutomationFixture setup("GraphEditorRequestsEach");
			const TypeRegistry& types = setup.GetEditor().GetTypeRegistry();
			const std::string graph(DoorPath);
			Run(setup, MakeGraphEditorRequest(types, GraphCreateParams{ .Path = graph, .Name = "Door" }), GraphEditorAction::CreateGraph);
			const uint32_t start = ReadNode(Run(setup, MakeGraphEditorRequest(types, AddParams("Event.OnStart", glm::vec2(0.0f))), GraphEditorAction::AddNode));
			const uint32_t log = ReadNode(Run(setup, MakeGraphEditorRequest(types, AddParams("Call.Log.Info", glm::vec2(240.0f, 0.0f), { { "Values", VariantValue(Json("hi")) } })),
				GraphEditorAction::AddNode));
			REQUIRE(start != 0);
			REQUIRE(log != 0);
			Run(setup, MakeGraphEditorRequest(types, GraphConnectParams{ .Graph = graph, .From = PinOf(start, "Then"), .To = PinOf(log, "In") }), GraphEditorAction::Connect);
			Run(setup, MakeGraphEditorRequest(types, GraphSetValuesParams{ .Graph = graph, .Values = { GraphValueEdit{ .Node = log, .Pin = "Values", .Value = VariantValue(Json("hello")) } } }),
				GraphEditorAction::SetValues);
			const GraphField speed{ .Name = "Speed", .Kind = GraphValueKind::Number, .Default = VariantValue(Json(2)), .Element = GraphValueKind::Number, .AssetType = {}, .EnumValues = {}, .Options = {} };
			Run(setup, MakeGraphEditorRequest(types, GraphSetMembersParams{ .Graph = graph, .Name = {}, .Rename = {}, .Remove = {}, .Fields = { speed }, .Variables = {} }),
				GraphEditorAction::SetMembers);
			Run(setup, MakeGraphEditorRequest(types, GraphMoveNodesParams{ .Graph = graph, .Positions = { GraphNodePosition{ .Node = log, .Position = glm::vec2(320.0f, 64.0f) } } }),
				GraphEditorAction::MoveNodes);

			Json document = ReadDocument(setup);
			CHECK(document["Name"] == "Door");
			REQUIRE(document["Nodes"].size() == 2);
			CHECK(document["Nodes"][1]["Position"] == Json::array({ 320, 64 }));
			CHECK(document["Nodes"][1]["Values"]["Values"] == "hello");
			CHECK(document["Links"] == Json::array({ LinkJson(start, "Then", log, "In") }));
			REQUIRE(document["Fields"].size() == 1);
			CHECK(document["Fields"][0]["Name"] == "Speed");

			Run(setup, MakeGraphEditorRequest(types, GraphDisconnectParams{ .Graph = graph, .From = PinOf(start, "Then"), .To = PinOf(log, "In") }), GraphEditorAction::Disconnect);
			CHECK(ReadDocument(setup)["Links"].empty());
			Run(setup, MakeGraphEditorRequest(types, GraphRemoveNodesParams{ .Graph = graph, .Nodes = { log } }), GraphEditorAction::RemoveNodes);
			CHECK(ReadDocument(setup)["Nodes"].size() == 1);

			// Undo and Redo are edit.undo and edit.redo of one step.
			const GraphEditorRequest undo = MakeGraphUndoRequest();
			CHECK(undo.Action == GraphEditorAction::Undo);
			CHECK(undo.Method == "edit.undo");
			REQUIRE(setup.Call(undo.Method, undo.Params));
			CHECK(ReadDocument(setup)["Nodes"].size() == 2);
			const GraphEditorRequest redo = MakeGraphRedoRequest();
			CHECK(redo.Action == GraphEditorAction::Redo);
			CHECK(redo.Method == "edit.redo");
			REQUIRE(setup.Call(redo.Method, redo.Params));
			CHECK(ReadDocument(setup)["Nodes"].size() == 1);

			// A registry without the graph methods' types cannot write their params.
			const TypeRegistry empty;
			const Result<GraphEditorRequest> unregistered = MakeGraphEditorRequest(empty, GraphCreateParams{ .Path = graph, .Name = {} });
			REQUIRE_FALSE(unregistered.has_value());
			CHECK(unregistered.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("GraphEditorRequests: a link dropped on empty canvas adds the chosen node linked to it in one undo step")
		{
			Test::AutomationFixture setup("GraphEditorRequestsLinked");
			const TypeRegistry& types = setup.GetEditor().GetTypeRegistry();
			REQUIRE(setup.Call("graph.create", Json{ { "path", std::string(DoorPath) } }));
			const uint32_t start = ReadNode(Run(setup, MakeGraphEditorRequest(types, AddParams("Event.OnStart", glm::vec2(0.0f))), GraphEditorAction::AddNode));
			const std::string before = ReadDoorText(setup);

			// From an output: the new node's input.
			const Json linked = Run(setup, MakeGraphLinkedNodeRequest(types, AddParams("Call.Log.Info", glm::vec2(240.0f, 0.0f)), PinOf(start, "Then"), GraphPinDirection::Output, "In"),
				GraphEditorAction::AddLinkedNode);
			const uint32_t log = ReadNode(linked["results"][0]);
			REQUIRE(log != 0);
			CHECK(JsonReader(linked["undoIndex"]).ReadUInt32().value_or(0) > 0);
			CHECK(ReadDocument(setup)["Links"] == Json::array({ LinkJson(start, "Then", log, "In") }));

			// From an input: the new node's output feeds it.
			Run(setup, MakeGraphLinkedNodeRequest(types, AddParams("Number.Add", glm::vec2(0.0f, 200.0f)), PinOf(log, "Values"), GraphPinDirection::Input, "Result"),
				GraphEditorAction::AddLinkedNode);
			const Json document = ReadDocument(setup);
			REQUIRE(document["Nodes"].size() == 3);
			const uint32_t add = JsonReader(document["Nodes"][2]["Id"]).ReadUInt32().value_or(0);
			CHECK(document["Nodes"][2]["Kind"] == "Number.Add");
			CHECK(document["Links"][1] == LinkJson(add, "Result", log, "Values"));

			// Each is one undo step: two undo the file back.
			REQUIRE(setup.Call("edit.undo", Json{ { "steps", 2 } }));
			CHECK(ReadDoorText(setup) == before);
		}

		TEST_CASE("GraphEditorRequests: duplicating nodes copies their kinds, literals and the links between them in one undo step")
		{
			Test::AutomationFixture setup("GraphEditorRequestsDuplicate");
			const TypeRegistry& types = setup.GetEditor().GetTypeRegistry();
			const std::string graph(DoorPath);
			REQUIRE(setup.Call("graph.create", Json{ { "path", graph } }));
			const uint32_t start = ReadNode(Run(setup, MakeGraphEditorRequest(types, AddParams("Event.OnStart", glm::vec2(0.0f))), GraphEditorAction::AddNode));
			const uint32_t log = ReadNode(Run(setup, MakeGraphEditorRequest(types, AddParams("Call.Log.Info", glm::vec2(240.0f, 0.0f))), GraphEditorAction::AddNode));
			const uint32_t add = ReadNode(Run(setup, MakeGraphEditorRequest(types, AddParams("Number.Add", glm::vec2(0.0f, 160.0f), { { "A", VariantValue(Json(2)) } })),
				GraphEditorAction::AddNode));
			Run(setup, MakeGraphEditorRequest(types, GraphConnectParams{ .Graph = graph, .From = PinOf(start, "Then"), .To = PinOf(log, "In") }), GraphEditorAction::Connect);
			Run(setup, MakeGraphEditorRequest(types, GraphConnectParams{ .Graph = graph, .From = PinOf(add, "Result"), .To = PinOf(log, "Values") }), GraphEditorAction::Connect);
			const std::string before = ReadDoorText(setup);

			// The copy holds the chosen nodes and only the links between two of them; unknown ids are skipped.
			GraphReadResult read;
			REQUIRE(setup.GetEditor().GetTypeRegistry().FindStruct<GraphReadResult>()->FromJson(&read, JsonReader(*setup.Call("graph.read", Json{ { "graph", graph } })), {}));
			const uint32_t chosen[] = { log, add, 999 };
			const GraphDocument copied = CopyGraphNodes(read.Document, chosen);
			REQUIRE(copied.Nodes.size() == 2);
			REQUIRE(copied.Links.size() == 1);
			CHECK(copied.Links.front().From == PinOf(add, "Result"));

			const Json duplicated = Run(setup, MakeGraphDuplicateRequest(types, graph, copied, glm::vec2(32.0f, 48.0f), false), GraphEditorAction::DuplicateNodes);
			CHECK(JsonReader(duplicated["undoIndex"]).ReadUInt32().value_or(0) > 0);
			const Json document = ReadDocument(setup);
			REQUIRE(document["Nodes"].size() == 5);
			const Json& newLog = document["Nodes"][3];
			const Json& newAdd = document["Nodes"][4];
			CHECK(newLog["Kind"] == "Call.Log.Info");
			CHECK(newLog["Position"] == Json::array({ 272, 48 }));
			CHECK(newAdd["Kind"] == "Number.Add");
			CHECK(newAdd["Position"] == Json::array({ 32, 208 }));
			CHECK(newAdd["Values"]["A"] == 2);
			const uint32_t newLogId = JsonReader(newLog["Id"]).ReadUInt32().value_or(0);
			const uint32_t newAddId = JsonReader(newAdd["Id"]).ReadUInt32().value_or(0);
			REQUIRE(document["Links"].size() == 3);
			CHECK(document["Links"][2] == LinkJson(newAddId, "Result", newLogId, "Values"));
			REQUIRE(setup.Call("edit.undo", Json::object()));
			CHECK(ReadDoorText(setup) == before);

			// A paste is the same batch under its own label; nothing to copy, or more than one edit holds, is refused.
			const Result<GraphEditorRequest> paste = MakeGraphDuplicateRequest(types, graph, copied, glm::vec2(0.0f), true);
			REQUIRE(paste.has_value());
			CHECK(paste->Params["label"] == "Paste Graph Nodes");
			CHECK(MakeGraphDuplicateRequest(types, graph, GraphDocument{}, glm::vec2(0.0f), false).error().GetCode() == ErrorCode::InvalidArgument);
			GraphDocument many;
			for (uint32_t id = 1; id <= MaxGraphEditorBatchOps + 1; ++id)
				many.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Number.Add", .Position = glm::vec2(0.0f), .Values = {} });
			CHECK(MakeGraphDuplicateRequest(types, graph, many, glm::vec2(0.0f), false).error().GetCode() == ErrorCode::InvalidArgument);

			// A copy placed past the canvas's edge stays on it.
			GraphDocument edge;
			edge.Nodes.push_back(GraphNode{ .Id = 1, .Kind = "Number.Add", .Position = glm::vec2(MaxGraphPosition - 8.0f, 0.0f), .Values = {} });
			const Result<GraphEditorRequest> clamped = MakeGraphDuplicateRequest(types, graph, edge, glm::vec2(32.0f, 0.0f), false);
			REQUIRE(clamped.has_value());
			CHECK(clamped->Params["ops"][0]["params"]["position"][0] == MaxGraphPosition);
		}

		// W24.2's review: a link carried off its input onto another moves in one atomic edit.
		TEST_CASE("GraphEditorRequests: a link moved to another input is one undo step, and a refused move keeps the link where it was")
		{
			Test::AutomationFixture setup("GraphEditorRequestsMoveLink");
			const TypeRegistry& types = setup.GetEditor().GetTypeRegistry();
			const std::string graph(DoorPath);
			REQUIRE(setup.Call("graph.create", Json{ { "path", graph } }));
			const uint32_t add = ReadNode(Run(setup, MakeGraphEditorRequest(types, AddParams("Number.Add", glm::vec2(0.0f), { { "A", VariantValue(Json(2)) } })), GraphEditorAction::AddNode));
			const uint32_t first = ReadNode(Run(setup, MakeGraphEditorRequest(types, AddParams("Call.Log.Info", glm::vec2(240.0f, 0.0f))), GraphEditorAction::AddNode));
			const uint32_t second = ReadNode(Run(setup, MakeGraphEditorRequest(types, AddParams("Call.Log.Info", glm::vec2(240.0f, 160.0f))), GraphEditorAction::AddNode));
			const uint32_t branch = ReadNode(Run(setup, MakeGraphEditorRequest(types, AddParams("Flow.Branch", glm::vec2(240.0f, 320.0f))), GraphEditorAction::AddNode));
			Run(setup, MakeGraphEditorRequest(types, GraphConnectParams{ .Graph = graph, .From = PinOf(add, "Result"), .To = PinOf(first, "Values") }), GraphEditorAction::Connect);
			const std::string before = ReadDoorText(setup);
			const size_t steps = setup.GetEditor().GetHistory().GetUndoCount();
			const GraphLink link{ .From = PinOf(add, "Result"), .To = PinOf(first, "Values") };

			// Moved: the old input loses the link and the new one has it, in one undo step that takes both back.
			const Json moved = Run(setup, MakeGraphMoveLinkRequest(types, graph, link, PinOf(second, "Values")), GraphEditorAction::MoveLink);
			CHECK(JsonReader(moved["undoIndex"]).ReadUInt32().value_or(0) > 0);
			CHECK(ReadDocument(setup)["Links"] == Json::array({ LinkJson(add, "Result", second, "Values") }));
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == steps + 1);
			REQUIRE(setup.Call("edit.undo", Json::object()));
			CHECK(ReadDoorText(setup) == before);

			// Refused (a Number cannot feed a Bool): the disconnect is undone with it, so the link stays and no step is added.
			const Result<GraphEditorRequest> refused = MakeGraphMoveLinkRequest(types, graph, link, PinOf(branch, "Condition"));
			REQUIRE(refused.has_value());
			CHECK(refused->Params["label"] == "Move Graph Link");
			CHECK_FALSE(setup.Call(refused->Method, refused->Params).has_value());
			CHECK(ReadDoorText(setup) == before);
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == steps);
		}
	}

}
