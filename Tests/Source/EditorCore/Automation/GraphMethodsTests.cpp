#include "TestsPCH.h"

#include "EditorCore/Automation/GraphMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// graph.* (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decisions 12 and 13, stream G-C): the C++ round trips. The
// methods run over streams G-A's document and G-B's library, transpiler and importer, so the cases run from W24.1's
// integration, which also registers the methods.

namespace Engine {

	namespace {

		constexpr std::string_view DoorPath = "Assets/Scripts/Door.graph";

		Json CallOrFail(Test::AutomationFixture& setup, std::string_view method, const Json& params)
		{
			Result<Json> result = setup.Call(method, params);
			INFO(std::string(method));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

		std::string ReadProjectText(Test::AutomationFixture& setup, std::string_view path)
		{
			const VfsPath file = VfsPath::Create("project", path).value_or(VfsPath());
			return setup.GetEditor().GetVfs().ReadText(file).value_or(std::string());
		}

		std::string ReadDoorText(Test::AutomationFixture& setup)
		{
			return ReadProjectText(setup, DoorPath);
		}

		bool ProjectPathExists(Test::AutomationFixture& setup, std::string_view path)
		{
			return setup.GetEditor().GetVfs().Exists(VfsPath::Create("project", path).value_or(VfsPath()));
		}

		// Writes `graph` (canonical) over the Door graph's source through the editor's write path, which schedules its import.
		void WriteDoor(Test::AutomationFixture& setup, GraphDocument graph)
		{
			CanonicalizeGraph(graph);
			REQUIRE(ValidateGraphStructure(graph).has_value());
			const Result<std::string> text = GraphToText(graph, setup.GetEditor().GetTypeRegistry());
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const VfsPath path = VfsPath::Create("project", DoorPath).value_or(VfsPath());
			REQUIRE(setup.GetEditor().WriteProjectFile(path, std::as_bytes(std::span(*text))).has_value());
		}

		// The id graph.addNode gave a node of `kind`.
		uint32_t AddNode(Test::AutomationFixture& setup, const Json& graph, std::string_view kind, const Json& extra = Json::object())
		{
			Json params = extra;
			params["graph"] = graph;
			params["kind"] = std::string(kind);
			const Json added = CallOrFail(setup, "graph.addNode", params);
			return JsonReader(added["node"]).ReadUInt32().value_or(0);
		}

		Json Pin(uint32_t node, std::string_view pin)
		{
			return Json{ { "Node", node }, { "Pin", std::string(pin) } };
		}

		// A field or variable in graph.setMembers' wire spelling (the .graph file's).
		Json Member(std::string_view name, std::string_view kind, const Json& defaultValue, bool field)
		{
			Json member{ { "Name", std::string(name) }, { "Kind", std::string(kind) }, { "Default", defaultValue }, { "Element", "Number" }, { "AssetType", "" },
				{ "EnumValues", Json::array() } };
			if (field)
				member["Options"] = Json::object();
			return member;
		}

		// The node of `id` in a graph.read document.
		Json FindNode(const Json& document, uint32_t id)
		{
			for (const Json& node : document["Nodes"])
				if (node["Id"] == Json(id))
					return node;
			return Json();
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("GraphMethods: graph.create, graph.addNode, graph.connect, graph.setValues and graph.setMembers build a graph that imports as a behaviour")
		{
			Test::AutomationFixture setup("GraphBuild");
			const Json created = CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			CHECK(created["name"] == "Door"); // the file's stem
			CHECK(created["graph"]["type"] == "Script");
			const Json graph = std::string(DoorPath);

			CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "variables", Json::array({ { { "Name", "Opened" }, { "Kind", "Bool" }, { "Default", false }, { "Element", "Number" }, { "AssetType", "" }, { "EnumValues", Json::array() } } }) } });
			const uint32_t start = AddNode(setup, graph, "Event.OnStart");
			const uint32_t set = AddNode(setup, graph, "Self.Set.Opened");
			const uint32_t log = AddNode(setup, graph, "Call.Log.Info", Json{ { "position", Json::array({ 480, 0 }) } });
			CHECK(start == 1);
			CHECK(set == 2);
			CHECK(log == 3);
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(start, "Then") }, { "to", Pin(set, "In") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(set, "Then") }, { "to", Pin(log, "In") } });
			CallOrFail(setup, "graph.setValues", Json{ { "graph", graph }, { "values", Json::array({ { { "node", set }, { "pin", "Value" }, { "value", true } }, { { "node", log }, { "pin", "Values" }, { "value", "Opened" } } }) } });

			// The file is canonical and the graph imports as the Door behaviour with nothing to report.
			const Json read = CallOrFail(setup, "graph.read", Json{ { "graph", graph }, { "luau", true } });
			CHECK(read["document"]["Nodes"].size() == 3);
			CHECK(read["document"]["Links"].size() == 2);
			CHECK(read["diagnostics"].empty());
			CHECK(JsonReader(read["luau"]).ReadString().value_or(std::string()).find("Script.Define(\"Door\", Graph)") != std::string::npos);
			const Json info = CallOrFail(setup, "asset.info", Json{ { "asset", graph } });
			CHECK(info["asset"]["type"] == "Script");
			const Json fields = CallOrFail(setup, "script.fields", Json{ { "script", graph } });
			CHECK(fields["kind"] == "Behaviour");

			// Refusals write nothing: an unknown kind, a link between incompatible pins, a value its pin does not take.
			const std::string before = ReadDoorText(setup);
			CHECK_FALSE(setup.Call("graph.addNode", Json{ { "graph", graph }, { "kind", "Flow.Nope" } }).has_value());
			CHECK_FALSE(setup.Call("graph.connect", Json{ { "graph", graph }, { "from", Pin(start, "Then") }, { "to", Pin(log, "Values") } }).has_value());
			CHECK_FALSE(setup.Call("graph.setValues", Json{ { "graph", graph }, { "values", Json::array({ { { "node", set }, { "pin", "Value" }, { "value", "yes" } } }) } }).has_value());
			CHECK(ReadDoorText(setup) == before);
		}

		TEST_CASE("GraphMethods: every graph edit is one undo step that restores the file byte for byte, and a dry run writes nothing")
		{
			Test::AutomationFixture setup("GraphUndo");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) }, { "name", "Door" } });
			const Json graph = std::string(DoorPath);
			const std::string empty = ReadDoorText(setup);
			const Json added = CallOrFail(setup, "graph.addNode", Json{ { "graph", graph }, { "kind", "Event.OnStart" } });
			CHECK(JsonReader(added["undoIndex"]).ReadUInt32().value_or(0) > 0);
			const std::string withNode = ReadDoorText(setup);
			CHECK(withNode != empty);
			CallOrFail(setup, "edit.undo", Json::object());
			CHECK(ReadDoorText(setup) == empty);
			CallOrFail(setup, "edit.redo", Json::object());
			CHECK(ReadDoorText(setup) == withNode);

			const Json dry = CallOrFail(setup, "graph.moveNodes", Json{ { "graph", graph }, { "dryRun", true }, { "positions", Json::array({ { { "node", 1 }, { "position", Json::array({ 64, 64 }) } } }) } });
			CHECK(dry["undoIndex"] == 0);
			CHECK(ReadDoorText(setup) == withNode);

			// An atomic batch names an earlier op's node by its name.
			const Json batch = CallOrFail(setup, "edit.batch", Json{ { "label", "Add a log" }, { "ops", Json::array({
																											{ { "method", "graph.addNode" }, { "name", "log" }, { "params", Json{ { "graph", graph }, { "kind", "Call.Log.Info" } } } },
																											{ { "method", "graph.connect" }, { "params", Json{ { "graph", graph }, { "from", Pin(1, "Then") }, { "to", Json{ { "Node", Json{ { "$ref", "log.node" } } }, { "Pin", "In" } } } } } },
																										}) } });
			CHECK(batch["results"].size() == 2);
			CallOrFail(setup, "edit.undo", Json::object());
			CHECK(ReadDoorText(setup) == withNode);

			// graph.create is one edit of the graph, its .meta and the folders it makes: a dry run writes none of them, its undo
			// removes them all, and its redo writes both files again byte for byte.
			constexpr std::string_view GatePath = "Assets/Gates/Gate.graph";
			constexpr std::string_view GateMeta = "Assets/Gates/Gate.graph.meta";
			const Json dryCreate = CallOrFail(setup, "graph.create", Json{ { "path", std::string(GatePath) }, { "dryRun", true } });
			CHECK(dryCreate["undoIndex"] == 0);
			CHECK_FALSE(ProjectPathExists(setup, GatePath));
			CHECK_FALSE(ProjectPathExists(setup, GateMeta));
			CHECK_FALSE(ProjectPathExists(setup, "Assets/Gates"));
			const Json create = CallOrFail(setup, "graph.create", Json{ { "path", std::string(GatePath) } });
			CHECK(JsonReader(create["undoIndex"]).ReadUInt32().value_or(0) > 0);
			const std::string gate = ReadProjectText(setup, GatePath);
			const std::string gateMeta = ReadProjectText(setup, GateMeta);
			REQUIRE_FALSE(gate.empty());
			REQUIRE_FALSE(gateMeta.empty());
			CallOrFail(setup, "edit.undo", Json::object());
			CHECK_FALSE(ProjectPathExists(setup, GatePath));
			CHECK_FALSE(ProjectPathExists(setup, GateMeta));
			CHECK_FALSE(ProjectPathExists(setup, "Assets/Gates"));
			CallOrFail(setup, "edit.redo", Json::object());
			CHECK(ReadProjectText(setup, GatePath) == gate);
			CHECK(ReadProjectText(setup, GateMeta) == gateMeta);
		}

		TEST_CASE("GraphMethods: graph.read maps findings to nodes and graph.nodes lists the library with the graph's members")
		{
			Test::AutomationFixture setup("GraphRead");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const Json graph = std::string(DoorPath);
			CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "fields", Json::array({ { { "Name", "Speed" }, { "Kind", "Number" }, { "Default", 2 }, { "Element", "Number" }, { "AssetType", "" }, { "EnumValues", Json::array() }, { "Options", Json::object() } } }) } });
			const uint32_t start = AddNode(setup, graph, "Event.OnStart");
			const uint32_t shot = AddNode(setup, graph, "Call.Audio.PlayOneShot");
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(start, "Then") }, { "to", Pin(shot, "In") } });
			// The one-shot's clip is neither linked nor set: incomplete, reported at the node, and the graph does not import.
			const Json read = CallOrFail(setup, "graph.read", Json{ { "graph", graph }, { "luau", true } });
			REQUIRE(read["diagnostics"].size() == 1);
			CHECK(read["diagnostics"][0]["code"] == "GRAPH_INVALID");
			CHECK(read["diagnostics"][0]["node"] == shot);
			CHECK(read["diagnostics"][0]["incomplete"] == true);
			CHECK(read["luau"] == "");

			const Json flow = CallOrFail(setup, "graph.nodes", Json{ { "prefix", "Flow." } });
			CHECK(flow["nodes"].size() == 6); // Flow.Branch, Delay, ForEach, ForRange, IfNotNil and Sequence
			const Json members = CallOrFail(setup, "graph.nodes", Json{ { "graph", graph }, { "prefix", "Self." } });
			CHECK(members["nodes"].size() == 3); // Self.Entity, Self.Get.Speed, Self.Set.Speed
			CHECK_FALSE(setup.Call("graph.nodes", Json{ { "kinds", Json::array({ "Flow.Nope" }) } }).has_value());
		}

		TEST_CASE("GraphMethods: graph.read reports a statement no event reaches, and graph.nodes filters by exact kinds")
		{
			Test::AutomationFixture setup("GraphUnreachable");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const Json graph = std::string(DoorPath);
			const uint32_t start = AddNode(setup, graph, "Event.OnStart");
			const uint32_t reached = AddNode(setup, graph, "Call.Log.Info");
			const uint32_t stray = AddNode(setup, graph, "Call.Log.Info");
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(start, "Then") }, { "to", Pin(reached, "In") } });
			const Json read = CallOrFail(setup, "graph.read", Json{ { "graph", graph } });
			REQUIRE(read["diagnostics"].size() == 1);
			CHECK(read["diagnostics"][0]["code"] == "GRAPH_UNREACHABLE_NODE");
			CHECK(read["diagnostics"][0]["severity"] == "Warning");
			CHECK(read["diagnostics"][0]["node"] == stray);
			CHECK(read["diagnostics"][0]["jsonPointer"] == "/Nodes/2");
			CHECK(read["luau"] == ""); // without luau

			const Json kinds = CallOrFail(setup, "graph.nodes", Json{ { "kinds", Json::array({ "Number.Add", "Flow.Branch" }) } });
			REQUIRE(kinds["nodes"].size() == 2);
			CHECK(kinds["nodes"][0]["kind"] == "Flow.Branch"); // sorted by kind
			CHECK(kinds["nodes"][1]["kind"] == "Number.Add");
			CHECK(kinds["nodes"][1]["pure"] == true);
		}

		TEST_CASE("GraphMethods: graph.connect replaces a data input's and an exec output's link, and graph.setMembers renames a member with its nodes")
		{
			Test::AutomationFixture setup("GraphEdits");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const Json graph = std::string(DoorPath);
			CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "fields", Json::array({ Member("Speed", "Number", 2, true) }) }, { "variables", Json::array({ Member("Opened", "Bool", false, false) }) } });
			// Without a position a node goes one node width right of the rightmost node, at the top of the graph's bounds.
			const uint32_t start = AddNode(setup, graph, "Event.OnStart");
			const uint32_t first = AddNode(setup, graph, "Call.Log.Info", Json{ { "position", Json::array({ 100, -50 }) } });
			const uint32_t second = AddNode(setup, graph, "Call.Log.Info");
			const uint32_t speedA = AddNode(setup, graph, "Self.Get.Speed");
			const uint32_t speedB = AddNode(setup, graph, "Self.Get.Speed");
			const uint32_t add = AddNode(setup, graph, "Number.Add");
			const Json placed = CallOrFail(setup, "graph.read", Json{ { "graph", graph } })["document"];
			CHECK(FindNode(placed, start)["Position"] == Json::array({ 0, 0 }));
			CHECK(FindNode(placed, second)["Position"] == Json::array({ 340, -50 }));

			// An exec output and a data input hold one link: a new one replaces it.
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(start, "Then") }, { "to", Pin(first, "In") } });
			const Json exec = CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(start, "Then") }, { "to", Pin(second, "In") } });
			REQUIRE(exec["replaced"].size() == 1);
			CHECK(exec["replaced"][0] == Json{ { "From", Pin(start, "Then") }, { "To", Pin(first, "In") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(speedA, "Value") }, { "to", Pin(add, "A") } });
			const Json data = CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(speedB, "Value") }, { "to", Pin(add, "A") } });
			REQUIRE(data["replaced"].size() == 1);
			CHECK(data["replaced"][0]["From"] == Pin(speedA, "Value"));
			// A data output feeds several inputs, and linking what is already linked changes nothing.
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(speedB, "Value") }, { "to", Pin(add, "B") } });
			const Json again = CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(speedB, "Value") }, { "to", Pin(add, "B") } });
			CHECK(again["replaced"].empty());
			CHECK(again["undoIndex"] == 0);

			// A null value removes a literal; graph.disconnect removes a link.
			CallOrFail(setup, "graph.setValues", Json{ { "graph", graph }, { "values", Json::array({ { { "node", second }, { "pin", "Values" }, { "value", "Opened" } } }) } });
			CHECK(FindNode(CallOrFail(setup, "graph.read", Json{ { "graph", graph } })["document"], second)["Values"]["Values"] == "Opened");
			CallOrFail(setup, "graph.setValues", Json{ { "graph", graph }, { "values", Json::array({ { { "node", second }, { "pin", "Values" }, { "value", nullptr } } }) } });
			CHECK(FindNode(CallOrFail(setup, "graph.read", Json{ { "graph", graph } })["document"], second)["Values"].empty());
			const Json disconnected = CallOrFail(setup, "graph.disconnect", Json{ { "graph", graph }, { "from", Pin(speedB, "Value") }, { "to", Pin(add, "B") } });
			CHECK(JsonReader(disconnected["undoIndex"]).ReadUInt32().value_or(0) > 0);

			// A rename rewrites the member's nodes, keeping their ids and links; the behaviour's name changes with it.
			const Json renamed = CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "name", "Gate" }, { "rename", Json::array({ { { "from", "Speed" }, { "to", "Rate" } } }) } });
			CHECK(renamed["renamedNodes"] == 2);
			const Json document = CallOrFail(setup, "graph.read", Json{ { "graph", graph } })["document"];
			CHECK(document["Name"] == "Gate");
			CHECK(document["Fields"][0]["Name"] == "Rate");
			CHECK(FindNode(document, speedA)["Kind"] == "Self.Get.Rate");
			CHECK(FindNode(document, speedB)["Kind"] == "Self.Get.Rate");
			CHECK(document["Links"].size() == 2);
			// The same members again change nothing; an unused member is removed.
			const Json unchanged = CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "fields", Json::array({ Member("Rate", "Number", 2, true) }) } });
			CHECK(unchanged["undoIndex"] == 0);
			CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "remove", Json::array({ "Opened" }) } });
			CHECK(CallOrFail(setup, "graph.read", Json{ { "graph", graph } })["document"]["Variables"].empty());

			// Removing nodes removes their links, reported in the canonical order.
			const Json removed = CallOrFail(setup, "graph.removeNodes", Json{ { "graph", graph }, { "nodes", Json::array({ add, start }) } });
			REQUIRE(removed["removedLinks"].size() == 2);
			CHECK(removed["removedLinks"][0]["From"] == Pin(start, "Then"));
			CHECK(removed["removedLinks"][1]["From"] == Pin(speedB, "Value"));
			CHECK(CallOrFail(setup, "graph.read", Json{ { "graph", graph } })["document"]["Links"].empty());
		}

		TEST_CASE("GraphMethods: every graph method refuses bad params at their pointers and writes nothing")
		{
			Test::AutomationFixture setup("GraphRefusals");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const Json graph = std::string(DoorPath);
			CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "fields", Json::array({ Member("Speed", "Number", 2, true) }) }, { "variables", Json::array({ Member("Opened", "Bool", false, false), Member("Spin", "Quat", Json::array({ 0, 0, 0, 1 }), false) }) } });
			const uint32_t start = AddNode(setup, graph, "Event.OnStart");
			const uint32_t log = AddNode(setup, graph, "Call.Log.Info");
			const uint32_t later = AddNode(setup, graph, "Call.Log.Info");
			const uint32_t add = AddNode(setup, graph, "Number.Add");
			const uint32_t sum = AddNode(setup, graph, "Number.Add");
			const uint32_t negation = AddNode(setup, graph, "Logic.Not");
			const uint32_t speed = AddNode(setup, graph, "Self.Get.Speed");
			const uint32_t load = AddNode(setup, graph, "Call.Assets.Load");
			const uint32_t shot = AddNode(setup, graph, "Call.Audio.PlayOneShot");
			const uint32_t spin = AddNode(setup, graph, "Self.Set.Spin");
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(start, "Then") }, { "to", Pin(log, "In") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(log, "Then") }, { "to", Pin(later, "In") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(speed, "Value") }, { "to", Pin(add, "A") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(add, "Result") }, { "to", Pin(sum, "A") } });
			CallOrFail(setup, "script.write", Json{ { "path", "Assets/Scripts/Mover.luau" }, { "source", "return {}" } });
			const std::string before = ReadDoorText(setup);
			const size_t history = setup.GetEditor().GetHistory().GetUndoCount();

			const auto refused = [&setup](std::string_view method, const Json& params, std::string_view code, std::string_view pointer)
			{
				CAPTURE(std::string(method));
				CAPTURE(params.dump());
				Json response = setup.Request(method, params);
				REQUIRE(response.contains("error"));
				Json data = response["error"]["data"];
				CHECK(data["errorCode"] == Json(std::string(code)));
				CHECK(data["location"]["pointer"] == Json(std::string(pointer)));
				return data;
			};
			const auto with = [&graph](Json params)
			{
				params["graph"] = graph;
				return params;
			};

			// graph.create: the path below Assets/ with the .graph extension, new, and a name that is a graph identifier.
			refused("graph.create", Json{ { "path", "Scripts/Door.graph" } }, "InvalidArgument", "/path");
			refused("graph.create", Json{ { "path", "Assets/Scripts/Door.luau" } }, "InvalidArgument", "/path");
			refused("graph.create", Json{ { "path", std::string(DoorPath) } }, "InvalidArgument", "/path");
			refused("graph.create", Json{ { "path", "Assets/Scripts/Other.graph" }, { "name", "end" } }, "InvalidArgument", "/name");
			refused("graph.create", Json{ { "path", "Assets/Scripts/my-door.graph" } }, "InvalidArgument", "/name");

			// The graph reference: a missing asset, a built-in, and a Luau script, whose refusal names script.read.
			refused("graph.read", Json{ { "graph", "Assets/Scripts/Missing.graph" } }, "NotFound", "/graph");
			refused("graph.read", Json{ { "graph", "engine://Textures/SpriteSquare" } }, "InvalidArgument", "/graph");
			Json luau = refused("graph.read", Json{ { "graph", "Assets/Scripts/Mover.luau" } }, "InvalidArgument", "/graph");
			CHECK(JsonReader(luau["hint"]).ReadString().value_or(std::string()).find("script.read") != std::string::npos);

			// graph.nodes and graph.addNode: kinds, positions and literals.
			Json kind = refused("graph.nodes", Json{ { "kinds", Json::array({ "Flow.Branch", "Flow.Brench" }) } }, "InvalidArgument", "/kinds/1");
			REQUIRE(kind["issues"].size() == 1); // located once, with its suggestions
			CHECK(kind["issues"][0]["suggestions"][0] == "Flow.Branch");
			refused("graph.addNode", with({ { "kind", "Flow.Nope" } }), "InvalidArgument", "/kind");
			refused("graph.addNode", with({ { "kind", "Flow.Branch" }, { "position", Json::array({ 2.0e6, 0 }) } }), "InvalidArgument", "/position");
			refused("graph.addNode", with({ { "kind", "Call.Log.Info" }, { "values", Json{ { "Nope", 1 } } } }), "InvalidArgument", "/values/Nope");
			refused("graph.addNode", with({ { "kind", "Logic.Not" }, { "values", Json{ { "Value", "yes" } } } }), "InvalidArgument", "/values/Value");

			// graph.removeNodes and graph.moveNodes: ids the graph holds, once each, and positions on the canvas.
			refused("graph.removeNodes", with({ { "nodes", Json::array() } }), "InvalidArgument", "/nodes");
			refused("graph.removeNodes", with({ { "nodes", Json::array({ 99 }) } }), "InvalidArgument", "/nodes/0");
			refused("graph.removeNodes", with({ { "nodes", Json::array({ start, start }) } }), "InvalidArgument", "/nodes/1");
			refused("graph.moveNodes", with({ { "positions", Json::array() } }), "InvalidArgument", "/positions");
			refused("graph.moveNodes", with({ { "positions", Json::array({ { { "node", 99 }, { "position", Json::array({ 0, 0 }) } } }) } }), "InvalidArgument", "/positions/0/node");
			refused("graph.moveNodes", with({ { "positions", Json::array({ { { "node", start }, { "position", Json::array({ 0, 1.0e7 }) } } }) } }), "InvalidArgument",
				"/positions/0/position");

			// graph.connect: nodes, pins and directions in the wire's spelling, two different nodes, compatible pins, no cycle.
			refused("graph.connect", with({ { "from", Pin(99, "Then") }, { "to", Pin(log, "In") } }), "InvalidArgument", "/from/Node");
			refused("graph.connect", with({ { "from", Pin(start, "Then") }, { "to", Pin(99, "In") } }), "InvalidArgument", "/to/Node");
			refused("graph.connect", with({ { "from", Pin(start, "Than") }, { "to", Pin(log, "In") } }), "InvalidArgument", "/from/Pin");
			refused("graph.connect", with({ { "from", Pin(log, "In") }, { "to", Pin(later, "In") } }), "InvalidArgument", "/from/Pin");
			refused("graph.connect", with({ { "from", Pin(start, "Then") }, { "to", Pin(later, "Then") } }), "InvalidArgument", "/to/Pin");
			refused("graph.connect", with({ { "from", Pin(add, "Result") }, { "to", Pin(add, "B") } }), "InvalidArgument", "/to/Node");
			refused("graph.connect", with({ { "from", Pin(negation, "Result") }, { "to", Pin(sum, "B") } }), "InvalidArgument", "/to");
			Json optional = refused("graph.connect", with({ { "from", Pin(load, "Result") }, { "to", Pin(shot, "clip") } }), "InvalidArgument", "/to");
			CHECK(JsonReader(optional["hint"]).ReadString().value_or(std::string()).find("Flow.IfNotNil") != std::string::npos);
			refused("graph.connect", with({ { "from", Pin(sum, "Result") }, { "to", Pin(add, "B") } }), "InvalidArgument", "/to");  // a data cycle
			refused("graph.connect", with({ { "from", Pin(later, "Then") }, { "to", Pin(log, "In") } }), "InvalidArgument", "/to"); // an exec cycle
			refused("graph.disconnect", with({ { "from", Pin(start, "Then") }, { "to", Pin(later, "In") } }), "NotFound", "/to");

			// graph.setValues: a node, a data input of it and a literal its type takes.
			refused("graph.setValues", with({ { "values", Json::array() } }), "InvalidArgument", "/values");
			refused("graph.setValues", with({ { "values", Json::array({ { { "node", 99 }, { "pin", "Values" }, { "value", 1 } } }) } }), "InvalidArgument", "/values/0/node");
			refused("graph.setValues", with({ { "values", Json::array({ { { "node", log }, { "pin", "Then" }, { "value", 1 } } }) } }), "InvalidArgument", "/values/0/pin");
			refused("graph.setValues", with({ { "values", Json::array({ { { "node", log }, { "pin", "Values" }, { "value", "a" } }, { { "node", negation }, { "pin", "Value" }, { "value", 3 } } }) } }),
				"InvalidArgument", "/values/1/value");
			// The zero quaternion, which Quat.New refuses, and a number the file would hold as another (its float, 0.3).
			refused("graph.setValues", with({ { "values", Json::array({ { { "node", spin }, { "pin", "Value" }, { "value", Json::array({ 0, 0, 0, 0 }) } } }) } }), "InvalidArgument",
				"/values/0/value");
			refused("graph.setValues", with({ { "values", Json::array({ { { "node", sum }, { "pin", "B" }, { "value", 0.30000001 } } }) } }), "InvalidArgument", "/values/0/value");

			// graph.setMembers: names, members that exist, members no node uses, a field that is no variable, member rules and
			// a kind change a link still needs.
			refused("graph.setMembers", with({ { "name", "end" } }), "InvalidArgument", "/name");
			refused("graph.setMembers", with({ { "rename", Json::array({ { { "from", "Sped" }, { "to", "Rate" } } }) } }), "InvalidArgument", "/rename/0/from");
			refused("graph.setMembers", with({ { "rename", Json::array({ { { "from", "Speed" }, { "to", "Opened" } } }) } }), "InvalidArgument", "/rename/0/to");
			refused("graph.setMembers", with({ { "rename", Json::array({ { { "from", "Speed" }, { "to", "Entity" } } }) } }), "InvalidArgument", "/rename/0/to");
			Json used = refused("graph.setMembers", with({ { "remove", Json::array({ "Speed" }) } }), "InvalidArgument", "/remove/0");
			CHECK(JsonReader(used["hint"]).ReadString().value_or(std::string()).find("graph.removeNodes") != std::string::npos);
			refused("graph.setMembers", with({ { "remove", Json::array({ "Nope" }) } }), "InvalidArgument", "/remove/0");
			refused("graph.setMembers", with({ { "fields", Json::array({ Member("Opened", "Bool", false, true) }) } }), "InvalidArgument", "/fields/0/Name");
			refused("graph.setMembers", with({ { "variables", Json::array({ Member("Count", "Number", 0, false), Member("__Hidden", "Number", 0, false) }) } }),
				"InvalidArgument", "/variables/1/Name");
			refused("graph.setMembers", with({ { "fields", Json::array({ Member("Speed", "Bool", false, true) }) } }), "InvalidArgument", "/fields/0/Kind");

			CHECK(ReadDoorText(setup) == before);
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == history);
		}

		TEST_CASE("GraphMethods: a graph source that does not read fails every graph method with ImportFailed naming the file")
		{
			Test::AutomationFixture setup("GraphUnreadable");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const VfsPath path = VfsPath::Create("project", DoorPath).value_or(VfsPath());
			constexpr std::string_view Broken = R"({"Format": "Graph", "Version": 1, "Name": "Door", "Nodes": [{"Id": 0}]})";
			REQUIRE(setup.GetEditor().WriteProjectFile(path, std::as_bytes(std::span(Broken))).has_value());
			const Json position = Json::array({ 0, 0 });
			for (const auto& [method, params] : { std::pair{ "graph.read", Json::object() }, std::pair{ "graph.nodes", Json::object() },
					 std::pair{ "graph.addNode", Json{ { "kind", "Event.OnStart" } } }, std::pair{ "graph.removeNodes", Json{ { "nodes", Json::array({ 1 }) } } },
					 std::pair{ "graph.moveNodes", Json{ { "positions", Json::array({ { { "node", 1 }, { "position", position } } }) } } },
					 std::pair{ "graph.connect", Json{ { "from", Pin(1, "Then") }, { "to", Pin(2, "In") } } },
					 std::pair{ "graph.disconnect", Json{ { "from", Pin(1, "Then") }, { "to", Pin(2, "In") } } },
					 std::pair{ "graph.setValues", Json{ { "values", Json::array({ { { "node", 1 }, { "pin", "Values" }, { "value", 1 } } }) } } },
					 std::pair{ "graph.setMembers", Json{ { "name", "Gate" } } } })
			{
				CAPTURE(method);
				Json request = params;
				request["graph"] = std::string(DoorPath);
				Json response = setup.Request(method, request);
				REQUIRE(response.contains("error"));
				CHECK(response["error"]["data"]["errorCode"] == "ImportFailed");
				CHECK(response["error"]["data"]["location"]["file"] == Json(std::string(DoorPath)));
			}
			CHECK(ReadDoorText(setup) == Broken);
		}

		// Decision 13 and the lead's decision (d) of W24.1's review: the graph methods share the editor's one library, built at its
		// first use from the engine's frozen script API registry (the declarations the importer's library is built from) and
		// kept for the editor's lifetime, instead of building one per request.
		TEST_CASE("GraphMethods: the editor builds its graph node library once, from the engine's script API, and keeps it")
		{
			Test::AutomationFixture setup("GraphLibrary");
			const Result<const GraphNodeLibrary*> first = setup.GetEditor().GetGraphNodeLibrary();
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			const Result<const GraphNodeLibrary*> second = setup.GetEditor().GetGraphNodeLibrary();
			REQUIRE(second.has_value());
			CHECK(*first == *second);
			const Result<Scope<GraphNodeLibrary>> fresh = GraphNodeLibrary::Create(setup.GetEditor().GetEngine().GetScriptApiRegistry());
			REQUIRE_MESSAGE(fresh.has_value(), fresh.error().ToString());
			CHECK((*first)->GetFingerprint() == (*fresh)->GetFingerprint());
			// graph.nodes lists the library's kinds.
			const Json flow = CallOrFail(setup, "graph.nodes", Json{ { "prefix", "Flow." } });
			CHECK(flow["nodes"].size() == static_cast<size_t>(std::ranges::count_if((*first)->GetNodes(), [](const GraphNodeDescription& node)
			{
				return node.Kind.starts_with("Flow.");
			})));
		}

		// Decisions 7 and 12 (W24.1's review): a node several exec links enter (a merge) has its flow in a function of its own,
		// laid out two blocks deep; an edit that leaves it one link lays that flow out again at the link's depth. Every edit that
		// so nests a flow past the 64-block limit is refused and writes nothing: graph.disconnect and graph.removeNodes of one of
		// the links, and graph.connect of a link that replaces one.
		TEST_CASE("GraphMethods: an edit that nests a flow past the depth limit is refused, whether it links, unlinks or removes nodes, and writes nothing")
		{
			Test::AutomationFixture setup("GraphDepth");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const Json graph = std::string(DoorPath);
			// Start 1 enters the log 3 at once; start 2 enters it at the end of 40 nested branches (10 to 49), and 30 nested
			// branches (100 to 129) follow it, so its flow function nests them 31 blocks deep. Branch 4 is linked to nothing.
			GraphDocument document;
			document.Name = "Door";
			document.Nodes = { GraphNode{ .Id = 1, .Kind = "Event.OnStart" }, GraphNode{ .Id = 2, .Kind = "Event.OnStart" }, GraphNode{ .Id = 3, .Kind = "Call.Log.Info" },
				GraphNode{ .Id = 4, .Kind = "Flow.Branch" } };
			document.Links = { GraphLink{ .From = { 1, "Then" }, .To = { 3, "In" } }, GraphLink{ .From = { 2, "Then" }, .To = { 10, "In" } },
				GraphLink{ .From = { 49, "True" }, .To = { 3, "In" } }, GraphLink{ .From = { 3, "Then" }, .To = { 100, "In" } } };
			for (const auto& [first, last] : { std::pair<uint32_t, uint32_t>{ 10, 49 }, std::pair<uint32_t, uint32_t>{ 100, 129 } })
			{
				for (uint32_t id = first; id <= last; ++id)
				{
					document.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Flow.Branch" });
					if (id != last)
						document.Links.push_back(GraphLink{ .From = { id, "True" }, .To = { id + 1, "In" } });
				}
			}
			WriteDoor(setup, document);
			const Json read = CallOrFail(setup, "graph.read", Json{ { "graph", graph } });
			for (const Json& diagnostic : read["diagnostics"])
				CHECK(diagnostic["code"] != "GRAPH_INVALID");
			const std::string before = ReadDoorText(setup);
			const size_t history = setup.GetEditor().GetHistory().GetUndoCount();

			// Each edit leaves the log one link, from the 40th branch: the branches after it would start up to 70 blocks deep, the
			// 25th of them (node 124) the first past the limit.
			const auto refused = [&setup](std::string_view method, const Json& params, std::string_view pointer)
			{
				CAPTURE(std::string(method));
				Json response = setup.Request(method, params);
				REQUIRE(response.contains("error"));
				CHECK(response["error"]["data"]["errorCode"] == "InvalidArgument");
				CHECK(response["error"]["data"]["location"]["pointer"] == Json(std::string(pointer)));
				CHECK(JsonReader(response["error"]["data"]["detail"]).ReadString().value_or(std::string()).find("node 124") != std::string::npos);
			};
			refused("graph.disconnect", Json{ { "graph", graph }, { "from", Pin(1, "Then") }, { "to", Pin(3, "In") } }, "/to");
			refused("graph.removeNodes", Json{ { "graph", graph }, { "nodes", Json::array({ 1 }) } }, "/nodes");
			refused("graph.connect", Json{ { "graph", graph }, { "from", Pin(1, "Then") }, { "to", Pin(4, "In") } }, "/to");
			CHECK(ReadDoorText(setup) == before);
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == history);

			// An edit that keeps the merge is accepted: the unlinked branch goes.
			CallOrFail(setup, "graph.removeNodes", Json{ { "graph", graph }, { "nodes", Json::array({ 4 }) } });
		}

		// Decision 13 (W24.1's review): graph.read returns the latest import's compile finding of the text, mapped to its node
		// through the line map, while the check is of the file's current bytes. One statement reading a chain of 300 values
		// validates, and its text holds more locals than a Luau function may, so its import fails to compile.
		TEST_CASE("GraphMethods: graph.read maps the import's compile finding to the node whose line it names")
		{
			Test::AutomationFixture setup("GraphCompileFinding");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			GraphDocument document;
			document.Name = "Door";
			document.Fields = { GraphField{ .Name = "Base", .Kind = GraphValueKind::Number, .Default = VariantValue(Json(0)) } };
			document.Nodes = { GraphNode{ .Id = 1, .Kind = "Event.OnStart" }, GraphNode{ .Id = 2, .Kind = "Call.Log.Info" } };
			document.Links = { GraphLink{ .From = { 1, "Then" }, .To = { 2, "In" } } };
			constexpr uint32_t Last = 303;
			for (uint32_t id = 3; id < Last; ++id)
			{
				document.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Number.Add", .Values = { { "B", VariantValue(Json(1)) } } });
				document.Links.push_back(GraphLink{ .From = { id, "Result" }, .To = { id == 3 ? 2 : id - 1, id == 3 ? "Values" : "A" } });
			}
			document.Nodes.push_back(GraphNode{ .Id = Last, .Kind = "Self.Get.Base" });
			document.Links.push_back(GraphLink{ .From = { Last, "Value" }, .To = { Last - 1, "A" } });
			WriteDoor(setup, document);
			// The text the import compiled, transpiled against the editor's library (the graph's text is too large to come
			// back inline with luau).
			const Result<const GraphNodeLibrary*> library = setup.GetEditor().GetGraphNodeLibrary();
			REQUIRE_MESSAGE(library.has_value(), library.error().ToString());
			CanonicalizeGraph(document);
			const Result<GraphTranspileResult> text = TranspileGraph(document, **library, DoorPath);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());

			// The import runs when the graph is next loaded; it fails to compile, keeping its check.
			const Test::ExpectLog failed(LogLevel::Error, "Out of local registers");
			CHECK(setup.Request("asset.reimport", Json{ { "asset", std::string(DoorPath) } }).contains("error"));
			const Json read = CallOrFail(setup, "graph.read", Json{ { "graph", std::string(DoorPath) } });
			CAPTURE(read.dump());
			REQUIRE(read["diagnostics"].size() == 1);
			const Json& finding = read["diagnostics"][0];
			CHECK(finding["code"] == "SCRIPT_COMPILE_ERROR");
			CHECK(finding["severity"] == "Error");
			CHECK(finding["jsonPointer"] == "");
			const uint32_t line = JsonReader(finding["line"]).ReadUInt32().value_or(0);
			REQUIRE(line > 0);
			const uint32_t node = JsonReader(finding["node"]).ReadUInt32().value_or(0);
			CHECK(node >= 3);
			CHECK(node == FindGraphNodeAtLine(*text, line));
		}

		// The documented refusals of a graph that holds the most nodes or has no id left, and of a graph reference that names a
		// glTF's sub-asset or a dependency file, which names no asset (W24.1's review).
		TEST_CASE("GraphMethods: graph.addNode refuses a full graph, and a graph reference refuses a sub-asset and finds no dependency file, writing nothing")
		{
			Test::AutomationFixture setup("GraphFull");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const Json graph = std::string(DoorPath);
			const auto refusedAdd = [&setup, &graph]()
			{
				const std::string before = ReadDoorText(setup);
				const size_t history = setup.GetEditor().GetHistory().GetUndoCount();
				Json response = setup.Request("graph.addNode", Json{ { "graph", graph }, { "kind", "Event.OnStart" } });
				REQUIRE(response.contains("error"));
				CHECK(response["error"]["data"]["errorCode"] == "Validation");
				CHECK(ReadDoorText(setup) == before);
				CHECK(setup.GetEditor().GetHistory().GetUndoCount() == history);
			};
			// MaxGraphNodes nodes.
			GraphDocument full;
			full.Name = "Door";
			for (uint32_t id = 1; id <= MaxGraphNodes; ++id)
				full.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Self.Entity" });
			WriteDoor(setup, full);
			refusedAdd();
			// One node whose id is the highest a node may have.
			GraphDocument last;
			last.Name = "Door";
			last.Nodes = { GraphNode{ .Id = MaxGraphNodeId, .Kind = "Self.Entity" } };
			WriteDoor(setup, last);
			refusedAdd();

			// A glTF's mesh (a sub-asset) and its buffer (a dependency file) are no graphs.
			for (const std::string_view file : { "Textured.gltf", "Textured.bin", "Textures/Checker.png" })
			{
				Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/" + std::string(file)));
				REQUIRE(bytes.has_value());
				const VfsPath path = VfsPath::Create("project", "Assets/Models/" + std::string(file)).value_or(VfsPath());
				REQUIRE(setup.GetEditor().CreateProjectDirectory(path.GetParent()).has_value());
				REQUIRE(setup.GetEditor().WriteProjectFile(path, *bytes).has_value());
			}
			CallOrFail(setup, "project.refreshAssets", Json::object());
			const Json meshes = CallOrFail(setup, "asset.list", Json{ { "type", "Mesh" } });
			REQUIRE_FALSE(meshes["assets"].empty());
			const std::string mesh = JsonReader(meshes["assets"][0]["path"]).ReadString().value_or(std::string());
			REQUIRE(mesh.find('#') != std::string::npos);
			Json subAsset = setup.Request("graph.read", Json{ { "graph", mesh } });
			REQUIRE(subAsset.contains("error"));
			CHECK(subAsset["error"]["data"]["errorCode"] == "InvalidArgument");
			CHECK(subAsset["error"]["data"]["location"]["pointer"] == "/graph");
			// A dependency file is no asset: neither its path nor its handle (its .meta's) names one.
			const Result<Json> bufferMeta = JsonReader::Parse(ReadProjectText(setup, "Assets/Models/Textured.bin.meta"));
			REQUIRE(bufferMeta.has_value());
			const std::string buffer = JsonReader((*bufferMeta)["Handle"]).ReadString().value_or(std::string());
			REQUIRE_FALSE(buffer.empty());
			for (const std::string& reference : { std::string("Assets/Models/Textured.bin"), buffer })
			{
				CAPTURE(reference);
				Json response = setup.Request("graph.read", Json{ { "graph", reference } });
				REQUIRE(response.contains("error"));
				CHECK(response["error"]["data"]["errorCode"] == "NotFound");
				CHECK(response["error"]["data"]["location"]["pointer"] == "/graph");
			}
		}

		TEST_CASE("GraphMethods: the Luau script methods refuse a graph's source and name the graph methods")
		{
			Test::AutomationFixture setup("GraphScriptMethods");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const std::string before = ReadDoorText(setup);
			const auto refused = [&setup](std::string_view method, const Json& params, std::string_view pointer, std::string_view named)
			{
				CAPTURE(std::string(method));
				Json response = setup.Request(method, params);
				REQUIRE(response.contains("error"));
				CHECK(response["error"]["data"]["errorCode"] == "InvalidArgument");
				CHECK(response["error"]["data"]["location"]["pointer"] == Json(std::string(pointer)));
				CHECK(JsonReader(response["error"]["data"]["hint"]).ReadString().value_or(std::string()).find(named) != std::string::npos);
			};
			refused("script.read", Json{ { "path", std::string(DoorPath) } }, "/path", "graph.read");
			refused("script.write", Json{ { "path", std::string(DoorPath) }, { "source", "return 1" } }, "/path", "graph.read");
			refused("script.create", Json{ { "path", "Assets/Scripts/Gate.graph" }, { "template", "Behaviour" } }, "/path", "graph.create");
			refused("script.check", Json{ { "paths", Json::array({ std::string(DoorPath) }) } }, "/paths/0", "graph.read");
			CHECK(ReadDoorText(setup) == before);
			// A graph is a Script asset all the same, whose fields script.fields reads once it imports.
			CHECK(CallOrFail(setup, "asset.info", Json{ { "asset", std::string(DoorPath) } })["asset"]["type"] == "Script");
		}
	}

}
