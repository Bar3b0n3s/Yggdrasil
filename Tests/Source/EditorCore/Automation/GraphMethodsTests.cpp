#include "TestsPCH.h"

#include "EditorCore/Automation/GraphMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
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
#include <vector>

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

		// The value of `node`'s output `pin` in a graph.liveValues result, or null.
		Json FindLiveValue(const Json& live, uint32_t node, std::string_view pin)
		{
			for (const Json& value : live["values"])
				if (value["node"] == Json(node) && value["pin"] == Json(std::string(pin)))
					return value;
			return Json();
		}

		// The ids of the Tagger's nodes.
		struct TaggerNodes
		{
			uint32_t Tick = 0;
			uint32_t Target = 0;
			uint32_t Tag = 0;
		};

		// The Tagger at `path`: each fixed step tags its Target field, which nothing sets, so its first step raises at the
		// AddTag node.
		TaggerNodes BuildTagger(Test::AutomationFixture& setup, const Json& path)
		{
			CallOrFail(setup, "graph.create", Json{ { "path", path } });
			CallOrFail(setup, "graph.setMembers", Json{ { "graph", path }, { "fields", Json::array({ Member("Target", "Entity", nullptr, true) }) } });
			const TaggerNodes nodes{ .Tick = AddNode(setup, path, "Event.OnFixedUpdate"),
				.Target = AddNode(setup, path, "Self.Get.Target"),
				.Tag = AddNode(setup, path, "Call.Entity.AddTag", Json{ { "values", Json{ { "tag", "Seen" } } } }) };
			CallOrFail(setup, "graph.connect", Json{ { "graph", path }, { "from", Pin(nodes.Tick, "Then") }, { "to", Pin(nodes.Tag, "In") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", path }, { "from", Pin(nodes.Target, "Value") }, { "to", Pin(nodes.Tag, "self") } });
			return nodes;
		}

		Json ScriptSlot(const Json& script)
		{
			return Json{ { "Script", script }, { "Fields", Json::object() }, { "ExecutionOrder", 0 } };
		}

		// The error graph.liveValues refuses `params` with ("errorCode", "location", "hint", ...).
		Json RefuseLiveValues(Test::AutomationFixture& setup, const Json& params, std::string_view code, std::string_view pointer)
		{
			CAPTURE(params.dump());
			Json response = setup.Request("graph.liveValues", params);
			REQUIRE(response.contains("error"));
			CHECK(response["error"]["data"]["errorCode"] == Json(std::string(code)));
			if (!pointer.empty())
				CHECK(response["error"]["data"]["location"]["pointer"] == Json(std::string(pointer)));
			return response["error"]["data"];
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
			CHECK(members["nodes"].size() == 4); // Self.Entity, Self.Get.Speed, Self.Instance (M39), Self.Set.Speed
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

		// M39 (Docs/Decisions/0056-m39-graphs-behaviours.md decision 8): a method's rename rewrites its Method and Return nodes,
		// keeping their ids, values and links, as a member's rename rewrites its Self nodes; removing a method in use is
		// refused with its nodes.
		TEST_CASE("GraphMethods: renaming a method rewrites its Method and Return nodes")
		{
			Test::AutomationFixture setup("GraphMethodRename");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const Json graph = std::string(DoorPath);
			const Json blocks{ { "Name", "Blocks" }, { "Parameters", Json::array({ Json{ { "Name", "hit" }, { "Kind", "Any" }, { "AssetType", "" } } }) },
				{ "Results", Json::array({ Json{ { "Kind", "Bool" }, { "AssetType", "" } } }) } };
			CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "methods", Json::array({ blocks }) } });
			const uint32_t event = AddNode(setup, graph, "Method.Blocks");
			const uint32_t returned = AddNode(setup, graph, "Return.Blocks", Json{ { "values", Json{ { "Value", true } } } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(event, "Then") }, { "to", Pin(returned, "In") } });

			const Json removal = setup.Request("graph.setMembers", Json{ { "graph", graph }, { "remove", Json::array({ "Blocks" }) } });
			REQUIRE(removal.contains("error"));
			CHECK(removal["error"]["data"]["location"]["pointer"] == "/remove/0");

			const Json renamed = CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "rename", Json::array({ { { "from", "Blocks" }, { "to", "Wards" } } }) } });
			CHECK(renamed["renamedNodes"] == 2);
			const Json read = CallOrFail(setup, "graph.read", Json{ { "graph", graph }, { "luau", true } });
			const Json& document = read["document"];
			CHECK(document["Methods"][0]["Name"] == "Wards");
			CHECK(FindNode(document, event)["Kind"] == "Method.Wards");
			CHECK(FindNode(document, returned)["Kind"] == "Return.Wards");
			CHECK(FindNode(document, returned)["Values"]["Value"] == true);
			CHECK(document["Links"].size() == 1);
			CHECK(read["diagnostics"].empty());
			CHECK(JsonReader(read["luau"]).ReadString().value_or(std::string()).find("function Graph.Wards(self: any, p_hit: any)") != std::string::npos);

			// A method's new name follows the method rules: not the class key, not another member's.
			for (const std::string_view to : { "RunsWhileWorldPaused", "OnStart" })
			{
				const Json refused = setup.Request("graph.setMembers", Json{ { "graph", graph }, { "rename", Json::array({ { { "from", "Wards" }, { "to", std::string(to) } } }) } });
				REQUIRE(refused.contains("error"));
				CHECK(refused["error"]["data"]["location"]["pointer"] == "/rename/0/to");
			}
		}

		// M39 (decision 8): graph.setMembers adds and replaces methods and sets the opt-in; a signature change a node still needs
		// is refused at the method; graph.nodes lists the new built-ins, the callbacks' Returns and a graph's own method kinds.
		TEST_CASE("GraphMethods: graph.setMembers sets methods and the world-pause opt-in, and graph.nodes lists the member and method kinds")
		{
			Test::AutomationFixture setup("GraphMethodMembers");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const Json graph = std::string(DoorPath);
			const auto method = [](std::string_view name, std::string_view parameterKind, std::string_view resultKind)
			{
				Json results = Json::array();
				if (!resultKind.empty())
					results.push_back(Json{ { "Kind", std::string(resultKind) }, { "AssetType", "" } });
				return Json{ { "Name", std::string(name) }, { "Parameters", Json::array({ Json{ { "Name", "amount" }, { "Kind", std::string(parameterKind) }, { "AssetType", "" } } }) },
					{ "Results", results } };
			};
			const Json set = CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "methods", Json::array({ method("Damaged", "Number", ""), method("Blocks", "Any", "Bool") }) }, { "runsWhileWorldPaused", true } });
			CHECK(JsonReader(set["undoIndex"]).ReadUInt32().value_or(0) > 0);
			Json document = CallOrFail(setup, "graph.read", Json{ { "graph", graph } })["document"];
			CHECK(document["RunsWhileWorldPaused"] == true);
			REQUIRE(document["Methods"].size() == 2);
			CHECK(document["Methods"][0]["Name"] == "Blocks"); // sorted by name
			CHECK(document["Methods"][1]["Results"].empty());
			// Absent, the opt-in stays; the same members again change nothing.
			const Json same = CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "methods", Json::array({ method("Blocks", "Any", "Bool") }) } });
			CHECK(same["undoIndex"] == 0);

			// graph.nodes: the built-ins and the callbacks' Returns always; the graph's Method and Return kinds with the graph.
			const Json nodes = CallOrFail(setup, "graph.nodes", Json{ { "kinds", Json::array({ "Return.OnUiClick", "Script.Call", "Script.Get", "Script.Set", "Self.Instance" }) } });
			CHECK(nodes["nodes"].size() == 5);
			CHECK_FALSE(setup.Call("graph.nodes", Json{ { "kinds", Json::array({ "Method.Blocks" }) } }).has_value());
			const Json own = CallOrFail(setup, "graph.nodes", Json{ { "graph", graph }, { "prefix", "Method." } });
			REQUIRE(own["nodes"].size() == 2);
			CHECK(own["nodes"][0]["kind"] == "Method.Blocks");
			CHECK(own["nodes"][0]["category"] == "Event");
			CHECK(CallOrFail(setup, "graph.nodes", Json{ { "graph", graph }, { "prefix", "Return.Damaged" } })["nodes"].size() == 1);

			// A signature change is refused while a link needs the old one: the Blocks result feeds a Bool input.
			const uint32_t event = AddNode(setup, graph, "Method.Blocks");
			const uint32_t branch = AddNode(setup, graph, "Flow.Branch");
			const uint32_t equal = AddNode(setup, graph, "Compare.Less", Json{ { "values", Json{ { "B", 1 } } } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(event, "Then") }, { "to", Pin(branch, "In") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(event, "amount") }, { "to", Pin(equal, "A") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(equal, "Result") }, { "to", Pin(branch, "Condition") } });
			const std::string before = ReadDoorText(setup);
			const Json refused = setup.Request("graph.setMembers", Json{ { "graph", graph }, { "methods", Json::array({ method("Blocks", "String", "Bool") }) } });
			REQUIRE(refused.contains("error"));
			CHECK(refused["error"]["data"]["location"]["pointer"] == "/methods/0/Parameters");
			CHECK(ReadDoorText(setup) == before);
			// And at /methods/<i>/Results while a Return node's literal or link needs the old result (the review of W39.1): a
			// Return.Blocks holding the literal true, another fed by a Bool output; a kind change and the result's removal.
			const uint32_t literal = AddNode(setup, graph, "Return.Blocks", Json{ { "values", Json{ { "Value", true } } } });
			const uint32_t fed = AddNode(setup, graph, "Return.Blocks");
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(equal, "Result") }, { "to", Pin(fed, "Value") } });
			const std::string withReturns = ReadDoorText(setup);
			for (const Json& changed : { method("Blocks", "Any", "Number"), method("Blocks", "Any", "") })
			{
				CAPTURE(changed.dump());
				const Json result = setup.Request("graph.setMembers", Json{ { "graph", graph }, { "methods", Json::array({ changed }) } });
				REQUIRE(result.contains("error"));
				CHECK(result["error"]["data"]["location"]["pointer"] == "/methods/0/Results");
				CHECK(ReadDoorText(setup) == withReturns);
			}
			// Without the literal's Return, the link alone (an issue at /Links/<i>, whose end is the Return) needs the Bool.
			CallOrFail(setup, "graph.removeNodes", Json{ { "graph", graph }, { "nodes", Json::array({ literal }) } });
			const std::string linkOnly = ReadDoorText(setup);
			const Json linked = setup.Request("graph.setMembers", Json{ { "graph", graph }, { "methods", Json::array({ method("Blocks", "Any", "String") }) } });
			REQUIRE(linked.contains("error"));
			CHECK(linked["error"]["data"]["location"]["pointer"] == "/methods/0/Results");
			CHECK(ReadDoorText(setup) == linkOnly);
			// A member may not change between a field and a method but by its removal and addition.
			const Json field = setup.Request("graph.setMembers", Json{ { "graph", graph }, { "fields", Json::array({ Member("Blocks", "Number", 0, true) }) } });
			REQUIRE(field.contains("error"));
			CHECK(field["error"]["data"]["location"]["pointer"] == "/fields/0/Name");
			// runsWhileWorldPaused false clears the opt-in.
			CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "runsWhileWorldPaused", false } });
			CHECK(CallOrFail(setup, "graph.read", Json{ { "graph", graph } })["document"]["RunsWhileWorldPaused"] == false);
		}

		// M39 (departure 1, the lead's approval): each method is a free-form value read strictly as a GraphMethod, refused at its
		// pointer when it is not one or breaks a method rule, a second result at its own pointer (departure 2); nothing is written.
		TEST_CASE("GraphMethods: graph.setMembers refuses a malformed method at its pointer")
		{
			Test::AutomationFixture setup("GraphMethodRefusals");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const Json graph = std::string(DoorPath);
			const std::string before = ReadDoorText(setup);
			const Json valid{ { "Name", "Pull" }, { "Parameters", Json::array() }, { "Results", Json::array() } };
			const auto refuse = [&setup, &graph](const Json& methods, std::string_view pointer)
			{
				CAPTURE(methods.dump());
				const Json response = setup.Request("graph.setMembers", Json{ { "graph", graph }, { "methods", methods } });
				REQUIRE(response.contains("error"));
				CHECK(response["error"]["data"]["errorCode"] == "InvalidArgument");
				CHECK(response["error"]["data"]["location"]["pointer"] == Json(std::string(pointer)));
			};
			refuse(Json::array({ valid, "Pull" }), "/methods/1");
			Json unknown = valid;
			unknown["Returns"] = true;
			refuse(Json::array({ unknown }), "/methods/0/Returns");
			Json kind = valid;
			kind["Parameters"] = Json::array({ Json{ { "Name", "hit" }, { "Kind", "Thing" }, { "AssetType", "" } } });
			refuse(Json::array({ kind }), "/methods/0/Parameters/0/Kind");
			Json results = valid;
			results["Results"] = Json::array({ Json{ { "Kind", "Bool" }, { "AssetType", "" } }, Json{ { "Kind", "Number" }, { "AssetType", "" } } });
			refuse(Json::array({ results }), "/methods/0/Results/1");
			Json named = valid;
			named["Name"] = "RunsWhileWorldPaused";
			refuse(Json::array({ named }), "/methods/0/Name");
			Json enumeration = valid;
			enumeration["Parameters"] = Json::array({ Json{ { "Name", "mode" }, { "Kind", "Enum" }, { "AssetType", "" } } });
			refuse(Json::array({ enumeration }), "/methods/0/Parameters/0/Kind");
			CHECK(ReadDoorText(setup) == before);
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
		// first use from the engine's frozen script API registry (the library the importer builds from the load-time API) and
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
			// The imports build theirs from the editor's load-time script API (Docs/Decisions/0041-script-check-performance.md
			// decision 3), which gives the same library, so the text an import compiles is the text graph.read and the export's
			// scan transpile (decision 24).
			const Result<Scope<GraphNodeLibrary>> imports = GraphNodeLibrary::Create(setup.GetEditor().GetLoadTimeScriptApi());
			REQUIRE_MESSAGE(imports.has_value(), imports.error().ToString());
			CHECK((*first)->GetFingerprint() == (*imports)->GetFingerprint());
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

			// M24's review (decision 13, Architecture §13.5): the import's findings are reported only while that import is of the
			// file's current bytes. A fixed graph written while reimports are deferred (as a lockstep or test session defers
			// them) was never imported, so its read reports no compile error; the failed import's own bytes bring it back.
			setup.GetEditor().GetAssets().SetReloadsDeferred(true);
			GraphDocument fixed;
			fixed.Name = "Door";
			fixed.Nodes = { GraphNode{ .Id = 1, .Kind = "Event.OnStart" }, GraphNode{ .Id = 2, .Kind = "Call.Log.Info" } };
			fixed.Links = { GraphLink{ .From = { 1, "Then" }, .To = { 2, "In" } } };
			WriteDoor(setup, fixed);
			const Json afterFix = CallOrFail(setup, "graph.read", Json{ { "graph", std::string(DoorPath) } });
			CAPTURE(afterFix.dump());
			CHECK(afterFix["diagnostics"].empty());
			WriteDoor(setup, document);
			const Json again = CallOrFail(setup, "graph.read", Json{ { "graph", std::string(DoorPath) } });
			REQUIRE(again["diagnostics"].size() == 1);
			CHECK(again["diagnostics"][0]["code"] == "SCRIPT_COMPILE_ERROR");
			CHECK(again["diagnostics"][0]["node"] == node);
		}

		// M24's review: a literal on a pin the node's kind no longer has (a binding's parameter renamed or removed, decision 10)
		// keeps the graph GRAPH_INVALID; graph.setValues with null removes it, as it removes one on a pin the kind has.
		TEST_CASE("GraphMethods: graph.setValues with null removes a literal on a pin its node's kind no longer has, in one undo step")
		{
			Test::AutomationFixture setup("GraphStaleLiteral");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const Json graph = std::string(DoorPath);
			GraphDocument document;
			document.Name = "Door";
			document.Nodes = { GraphNode{ .Id = 1, .Kind = "Event.OnStart" },
				GraphNode{ .Id = 2, .Kind = "Call.Log.Info", .Values = { { "Values", VariantValue(Json("Opened")) }, { "clip", VariantValue(Json("Assets/Old.wav")) } } } };
			document.Links = { GraphLink{ .From = { 1, "Then" }, .To = { 2, "In" } } };
			WriteDoor(setup, document);
			const Json before = CallOrFail(setup, "graph.read", Json{ { "graph", graph } });
			REQUIRE(before["diagnostics"].size() == 1);
			CHECK(before["diagnostics"][0]["code"] == "GRAPH_INVALID");
			CHECK(before["diagnostics"][0]["incomplete"] == false);
			CHECK(before["diagnostics"][0]["jsonPointer"] == "/Nodes/1/Values/clip");
			const std::string stale = ReadDoorText(setup);

			// A value on that pin, and a null on a pin that holds no literal, are refused at the pin, writing nothing.
			for (const Json& value : { Json("Assets/New.wav"), Json() })
			{
				const std::string pin = value.is_null() ? "nope" : "clip";
				Json response = setup.Request("graph.setValues", Json{ { "graph", graph }, { "values", Json::array({ { { "node", 2 }, { "pin", pin }, { "value", value } } }) } });
				REQUIRE(response.contains("error"));
				CHECK(response["error"]["data"]["location"]["pointer"] == "/values/0/pin");
			}
			CHECK(ReadDoorText(setup) == stale);

			const Json cleared = CallOrFail(setup, "graph.setValues", Json{ { "graph", graph }, { "values", Json::array({ { { "node", 2 }, { "pin", "clip" }, { "value", nullptr } } }) } });
			CHECK(JsonReader(cleared["undoIndex"]).ReadUInt32().value_or(0) > 0);
			const Json after = CallOrFail(setup, "graph.read", Json{ { "graph", graph } });
			CHECK(after["diagnostics"].empty());
			CHECK(FindNode(after["document"], 2)["Values"] == Json{ { "Values", "Opened" } });
			CallOrFail(setup, "edit.undo", Json::object());
			CHECK(ReadDoorText(setup) == stale);
		}

		// GraphMethods.h and Architecture §13.5: a later entry for the same pin (graph.setValues) or node (graph.moveNodes) wins.
		TEST_CASE("GraphMethods: a later entry of graph.setValues for a pin, and of graph.moveNodes for a node, wins")
		{
			Test::AutomationFixture setup("GraphLaterEntry");
			CallOrFail(setup, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const Json graph = std::string(DoorPath);
			const uint32_t log = AddNode(setup, graph, "Call.Log.Info");
			CallOrFail(setup, "graph.setValues", Json{ { "graph", graph }, { "values", Json::array({ { { "node", log }, { "pin", "Values" }, { "value", "first" } }, { { "node", log }, { "pin", "Values" }, { "value", "second" } } }) } });
			CallOrFail(setup, "graph.moveNodes", Json{ { "graph", graph }, { "positions", Json::array({ { { "node", log }, { "position", Json::array({ 10, 10 }) } }, { { "node", log }, { "position", Json::array({ 20, 30 }) } } }) } });
			const Json node = FindNode(CallOrFail(setup, "graph.read", Json{ { "graph", graph } })["document"], log);
			CHECK(node["Values"] == Json{ { "Values", "second" } });
			CHECK(node["Position"] == Json::array({ 20, 30 }));
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

		// graph.liveValues (decision 14, W24.2's stream B): a play session's graph behaviour, read between frames.
		TEST_CASE("GraphMethods: graph.liveValues reads a playing graph's node values, members and errors mapped to nodes, and the state hash is the one without the read")
		{
			Test::AutomationFixture setup("GraphLiveValues");
			// The Counter: each fixed step adds its Step field to its Count variable, then moves its entity to x = Count, so the
			// state hash, which covers the Transform but no script table, sees every Count (W24.2's review: a read that changed
			// the instance changes the hash).
			const Json counter = "Assets/Scripts/Counter.graph";
			CallOrFail(setup, "graph.create", Json{ { "path", counter } });
			CallOrFail(setup, "graph.setMembers", Json{ { "graph", counter }, { "fields", Json::array({ Member("Step", "Number", 1, true) }) }, { "variables", Json::array({ Member("Count", "Number", 0, false) }) } });
			const uint32_t step = AddNode(setup, counter, "Event.OnFixedUpdate");
			const uint32_t count = AddNode(setup, counter, "Self.Get.Count");
			const uint32_t increment = AddNode(setup, counter, "Self.Get.Step");
			const uint32_t add = AddNode(setup, counter, "Number.Add");
			const uint32_t store = AddNode(setup, counter, "Self.Set.Count");
			const uint32_t self = AddNode(setup, counter, "Self.Entity");
			const uint32_t transform = AddNode(setup, counter, "Get.Entity.Transform");
			const uint32_t counted = AddNode(setup, counter, "Self.Get.Count");
			const uint32_t place = AddNode(setup, counter, "Vector.Make", Json{ { "values", Json{ { "Y", 0 }, { "Z", 0 } } } });
			const uint32_t move = AddNode(setup, counter, "Set.Transform.Translation");
			CallOrFail(setup, "graph.connect", Json{ { "graph", counter }, { "from", Pin(step, "Then") }, { "to", Pin(store, "In") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", counter }, { "from", Pin(count, "Value") }, { "to", Pin(add, "A") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", counter }, { "from", Pin(increment, "Value") }, { "to", Pin(add, "B") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", counter }, { "from", Pin(add, "Result") }, { "to", Pin(store, "Value") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", counter }, { "from", Pin(store, "Then") }, { "to", Pin(move, "In") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", counter }, { "from", Pin(self, "Value") }, { "to", Pin(transform, "self") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", counter }, { "from", Pin(transform, "Value") }, { "to", Pin(move, "self") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", counter }, { "from", Pin(counted, "Value") }, { "to", Pin(place, "X") } });
			CallOrFail(setup, "graph.connect", Json{ { "graph", counter }, { "from", Pin(place, "Result") }, { "to", Pin(move, "Value") } });
			REQUIRE(CallOrFail(setup, "graph.read", Json{ { "graph", counter } })["diagnostics"].empty());
			const Json tagger = "Assets/Scripts/Tagger.graph";
			const TaggerNodes taggerNodes = BuildTagger(setup, tagger);
			REQUIRE(CallOrFail(setup, "graph.read", Json{ { "graph", tagger } })["diagnostics"].empty());

			CallOrFail(setup, "entity.create", Json{ { "name", "Counter" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ ScriptSlot(counter), ScriptSlot(tagger) }) } } } } } });
			CallOrFail(setup, "entity.create", Json{ { "name", "Plain" } });
			// Without a play session there is nothing to read.
			RefuseLiveValues(setup, Json{ { "entity", "/Counter" } }, "InvalidState", "");

			// Two lockstep plays of five ticks from one seed, the first reading live values after its second tick. Each play's
			// Tagger logs its one error at its first step.
			const auto run = [&](bool read)
			{
				const Test::ExpectLog tagged(LogLevel::Error, "attempt to index nil with 'AddTag'");
				CallOrFail(setup, "play.start", Json{ { "lockstep", true }, { "seed", 5 } });
				CallOrFail(setup, "play.step", Json{ { "ticks", 2 } });
				if (read)
				{
					const Json live = CallOrFail(setup, "graph.liveValues", Json{ { "entity", "/Counter" } });
					CHECK(live["graph"]["path"] == counter);
					CHECK(live["entity"]["path"] == "/Counter");
					CHECK(live["slot"] == 0);
					CHECK(live["tick"] == 2);
					CHECK(live["truncated"] == false);
					// Tick 2 read a Count of 1 and a Step of 1, stored their sum and moved the entity to x = 2. The values are sorted
					// by node and pin.
					CHECK(FindLiveValue(live, count, "Value") == Json{ { "node", count }, { "pin", "Value" }, { "type", "number" }, { "value", 1 } });
					CHECK(FindLiveValue(live, increment, "Value") == Json{ { "node", increment }, { "pin", "Value" }, { "type", "number" }, { "value", 1 } });
					CHECK(FindLiveValue(live, add, "Result") == Json{ { "node", add }, { "pin", "Result" }, { "type", "number" }, { "value", 2 } });
					CHECK(FindLiveValue(live, counted, "Value")["value"] == 2);
					CHECK(FindLiveValue(live, place, "Result")["value"] == Json::array({ 2, 0, 0 }));
					// The entity is an id; its Transform, a component proxy, has no JSON spelling and is null under its type.
					CHECK(FindLiveValue(live, self, "Value")["value"] == live["entity"]["id"]);
					CHECK(FindLiveValue(live, transform, "Value")["value"].is_null());
					CHECK(FindLiveValue(live, transform, "Value")["type"] == "Transform");
					CHECK(live["values"].size() == 7);
					std::vector<std::pair<uint32_t, std::string>> order;
					for (const Json& value : live["values"])
						order.emplace_back(JsonReader(value["node"]).ReadUInt32().value_or(0), JsonReader(value["pin"]).ReadString().value_or(std::string()));
					CHECK(std::ranges::is_sorted(order));
					CHECK(live["members"]
						== Json::array({ { { "name", "Count" }, { "type", "number" }, { "value", 2 } }, { { "name", "Step" }, { "type", "number" }, { "value", 1 } } }));
					CHECK(live["errors"].empty());

					// The second slot by its behaviour's name: its error, raised at its first step, is the AddTag node's.
					const Json faulty = CallOrFail(setup, "graph.liveValues", Json{ { "entity", "/Counter" }, { "script", "Tagger" } });
					CHECK(faulty["slot"] == 1);
					CHECK(faulty["values"].empty());
					CHECK(faulty["members"] == Json::array({ { { "name", "Target" }, { "type", "nil" }, { "value", nullptr } } }));
					REQUIRE(faulty["errors"].size() == 1);
					CHECK(faulty["errors"][0]["node"] == taggerNodes.Tag);
					CHECK(faulty["errors"][0]["line"] > 0);
					CHECK(faulty["errors"][0]["count"] == 1);
					const Json text = CallOrFail(setup, "graph.read", Json{ { "graph", tagger }, { "luau", true } });
					const uint32_t line = JsonReader(faulty["errors"][0]["line"]).ReadUInt32().value_or(0);
					REQUIRE(line >= 1);
					REQUIRE(line <= text["lineNodes"].size());
					CHECK(text["lineNodes"][line - 1] == taggerNodes.Tag);

					// The refusals: an entity the play scene does not hold, one with no graph slot, and a behaviour no slot has,
					// whose hint names the entity's graph behaviours.
					RefuseLiveValues(setup, Json{ { "entity", "/Nobody" } }, "NotFound", "/entity");
					RefuseLiveValues(setup, Json{ { "entity", "/Plain" } }, "NotFound", "/script");
					const Json unknown = RefuseLiveValues(setup, Json{ { "entity", "/Counter" }, { "script", "Door" } }, "NotFound", "/script");
					const std::string hint = JsonReader(unknown["hint"]).ReadString().value_or(std::string());
					CHECK(hint.contains("Counter"));
					CHECK(hint.contains("Tagger"));
				}
				const Json stepped = CallOrFail(setup, "play.step", Json{ { "ticks", 3 } });
				CallOrFail(setup, "play.stop", Json::object());
				return JsonReader(stepped["stateHash"]).ReadString().value_or(std::string());
			};
			const std::string read = run(true);
			CHECK_FALSE(read.empty());
			CHECK(read == run(false));
		}

		// W24.2's review: graph.liveValues' refusals in a simulation and for a graph slot whose source does not read.
		TEST_CASE("GraphMethods: graph.liveValues refuses a simulation, and a graph slot whose source does not read names no behaviour")
		{
			Test::AutomationFixture setup("GraphLiveValuesRefused");
			const Json gate = "Assets/Scripts/Gate.graph";
			const Json broken = "Assets/Scripts/Broken.graph";
			CallOrFail(setup, "graph.create", Json{ { "path", gate } });
			CallOrFail(setup, "graph.create", Json{ { "path", broken } });
			CallOrFail(setup, "entity.create", Json{ { "name", "Gate" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ ScriptSlot(gate), ScriptSlot(broken) }) } } } } } });
			CallOrFail(setup, "entity.create", Json{ { "name", "Broken" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ ScriptSlot(broken) }) } } } } } });

			// A simulation runs no behaviours, so there is nothing to read.
			CallOrFail(setup, "play.start", Json{ { "mode", "simulate" }, { "lockstep", true } });
			const Json simulated = RefuseLiveValues(setup, Json{ { "entity", "/Gate" } }, "InvalidState", "");
			CHECK(simulated.contains("hint"));
			CallOrFail(setup, "play.stop", Json::object());

			// During play, Broken's source stops reading (the session keeps running its old code: its reimport waits for the
			// play's end). Without a behaviour name the first graph slot is read, and its read fails; by name it names nothing.
			CallOrFail(setup, "play.start", Json{ { "lockstep", true } });
			CallOrFail(setup, "play.step", Json{ { "ticks", 1 } });
			const std::string brokenPath = JsonReader(broken).ReadString().value_or(std::string());
			const VfsPath path = VfsPath::Create("project", brokenPath).value_or(VfsPath());
			const std::string original = ReadProjectText(setup, brokenPath);
			constexpr std::string_view Unreadable = R"({"Format": "Graph", "Version": 1, "Name": "Broken", "Nodes": [{"Id": 0}]})";
			REQUIRE(setup.GetEditor().WriteProjectFile(path, std::as_bytes(std::span(Unreadable))).has_value());
			const Json failed = RefuseLiveValues(setup, Json{ { "entity", "/Broken" } }, "ImportFailed", "");
			CHECK(failed["location"]["file"] == broken);
			const Json named = RefuseLiveValues(setup, Json{ { "entity", "/Broken" }, { "script", "Broken" } }, "NotFound", "/script");
			CHECK_FALSE(JsonReader(named["hint"]).ReadString().value_or(std::string()).empty());
			// Gate's first slot still reads; by name, Broken is no behaviour of it now.
			CHECK(CallOrFail(setup, "graph.liveValues", Json{ { "entity", "/Gate" } })["graph"]["path"] == gate);
			const Json other = RefuseLiveValues(setup, Json{ { "entity", "/Gate" }, { "script", "Broken" } }, "NotFound", "/script");
			CHECK(JsonReader(other["hint"]).ReadString().value_or(std::string()).contains("Gate"));
			// The source restored before the play ends, so the reimport the play deferred imports it as it was.
			REQUIRE(setup.GetEditor().WriteProjectFile(path, std::as_bytes(std::span(original))).has_value());
			CallOrFail(setup, "play.stop", Json::object());
		}

		// W24.2's review (the lead's decision (d)): graph.liveValues keeps a graph's read between its calls, which the Graph Editor
		// makes once a frame, and reads it again whenever the graph changes. An edit during a lockstep play writes the source and
		// defers its reimport, so the session runs the old code while the method maps it through the edited text.
		TEST_CASE("GraphMethods: graph.liveValues follows an edit of its graph during play: the members and the errors' nodes are the edited graph's")
		{
			Test::AutomationFixture setup("GraphLiveValuesEdited");
			const Json tagger = "Assets/Scripts/Tagger.graph";
			const TaggerNodes nodes = BuildTagger(setup, tagger);
			CallOrFail(setup, "entity.create", Json{ { "name", "Tagged" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ ScriptSlot(tagger) }) } } } } } });
			const Test::ExpectLog raised(LogLevel::Error, "attempt to index nil with 'AddTag'");
			CallOrFail(setup, "play.start", Json{ { "lockstep", true } });
			CallOrFail(setup, "play.step", Json{ { "ticks", 2 } });
			// The node a line of the graph's current text is.
			const auto lineNode = [&setup, &tagger](uint32_t line)
			{
				const Json text = CallOrFail(setup, "graph.read", Json{ { "graph", tagger }, { "luau", true } });
				REQUIRE(line >= 1);
				REQUIRE(line <= text["lineNodes"].size());
				return JsonReader(text["lineNodes"][line - 1]).ReadUInt32().value_or(0);
			};
			const auto members = [](const Json& live)
			{
				std::vector<std::string> names;
				for (const Json& member : live["members"])
					names.push_back(JsonReader(member["name"]).ReadString().value_or(std::string()));
				return names;
			};

			const Json before = CallOrFail(setup, "graph.liveValues", Json{ { "entity", "/Tagged" } });
			REQUIRE(before["errors"].size() == 1);
			const uint32_t line = JsonReader(before["errors"][0]["line"]).ReadUInt32().value_or(0);
			CHECK(before["errors"][0]["node"] == nodes.Tag);
			CHECK(members(before) == std::vector<std::string>{ "Target" });
			// Read again unchanged: the same answer.
			CHECK(CallOrFail(setup, "graph.liveValues", Json{ { "entity", "/Tagged" } }) == before);

			// A variable added during play: one more line in OnCreate moves every later line down, so the error's line, still
			// the old code's, is now another node's in the edited text, and the new variable is a member the instance lacks.
			CallOrFail(setup, "graph.setMembers", Json{ { "graph", tagger }, { "variables", Json::array({ Member("Extra", "Number", 0, false) }) } });
			const Json edited = CallOrFail(setup, "graph.liveValues", Json{ { "entity", "/Tagged" } });
			REQUIRE(edited["errors"].size() == 1);
			CHECK(edited["errors"][0]["line"] == line);
			CHECK(edited["errors"][0]["node"] == lineNode(line));
			CHECK(edited["errors"][0]["node"] != nodes.Tag);
			CHECK(members(edited) == std::vector<std::string>{ "Extra", "Target" });

			// Undone, the graph reads as before.
			CallOrFail(setup, "edit.undo", Json::object());
			const Json undone = CallOrFail(setup, "graph.liveValues", Json{ { "entity", "/Tagged" } });
			CHECK(undone["errors"][0]["node"] == nodes.Tag);
			CHECK(members(undone) == std::vector<std::string>{ "Target" });
			CallOrFail(setup, "play.stop", Json::object());
		}
	}

}
