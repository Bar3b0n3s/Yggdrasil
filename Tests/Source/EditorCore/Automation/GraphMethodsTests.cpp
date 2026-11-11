#include "TestsPCH.h"

#include "EditorCore/Automation/GraphMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AutomationTestClient.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

// graph.* (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decisions 12 and 13, stream G-C): the C++ round trips. Skipped
// until stream G-C registers and implements the methods (with streams G-A's and G-B's document, library and importer).

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

		std::string ReadDoorText(Test::AutomationFixture& setup)
		{
			const VfsPath path = VfsPath::Create("project", DoorPath).value_or(VfsPath());
			return setup.GetEditor().GetVfs().ReadText(path).value_or(std::string());
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

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("GraphMethods: graph.create, graph.addNode, graph.connect, graph.setValues and graph.setMembers build a graph that imports as a behaviour"
			* doctest::skip(true))
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

		TEST_CASE("GraphMethods: every graph edit is one undo step that restores the file byte for byte, and a dry run writes nothing" * doctest::skip(true))
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
		}

		TEST_CASE("GraphMethods: graph.read maps findings to nodes and graph.nodes lists the library with the graph's members" * doctest::skip(true))
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
	}

}
