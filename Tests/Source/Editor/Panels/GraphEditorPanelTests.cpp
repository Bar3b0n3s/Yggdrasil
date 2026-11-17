#include "TestsPCH.h"
#include "Editor/Panels/GraphEditorPanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Graph/GraphEditorRequests.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Support/ExpectLog.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The graph editor's canvas (M24, W24.2's stream A; Docs/Decisions/0035-m24-contract.md decision 15): the window shows the open
// graph, and every action that changes it, driven with ordinary mouse and keyboard input, queues its graph method (or an edit
// method) for the editor's safe point, exactly as an agent calls it.

namespace Engine {

	namespace {

		constexpr std::string_view DoorPath = "Assets/Scripts/Door.graph";

		Json CallOrFail(Test::UtilityPanelFixture& fixture, std::string_view method, const Json& params)
		{
			Result<Json> result = fixture.GetClient().Call(method, params);
			INFO(std::string(method));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

		uint32_t AddNode(Test::UtilityPanelFixture& fixture, std::string_view kind, float x, float y)
		{
			const Json added = CallOrFail(fixture, "graph.addNode", Json{ { "graph", std::string(DoorPath) }, { "kind", std::string(kind) }, { "position", Json::array({ x, y }) } });
			return JsonReader(added["node"]).ReadUInt32().value_or(0);
		}

		Json ReadDocument(Test::UtilityPanelFixture& fixture)
		{
			return CallOrFail(fixture, "graph.read", Json{ { "graph", std::string(DoorPath) } })["document"];
		}

		// A project with the empty graph Door, open in `panel`.
		AssetHandle OpenDoor(Test::UtilityPanelFixture& fixture, GraphEditorPanel& panel)
		{
			fixture.OpenProject();
			CallOrFail(fixture, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const std::optional<AssetHandle> graph = fixture.GetEditor().GetAssets().Resolve(DoorPath);
			REQUIRE(graph.has_value());
			panel.Open(GraphEditorTarget{ .Graph = *graph, .Node = 0, .Line = 0, .JsonPointer = {}, .Finding = {} });
			return *graph;
		}

		// Draws and runs the queued requests until the panel shows what the methods give (graph.read, then graph.nodes).
		std::string Settle(Test::UtilityPanelFixture& fixture, GraphEditorPanel& panel)
		{
			for (int round = 0; round < 6; ++round)
			{
				static_cast<void>(fixture.Draw(panel));
				fixture.Pump();
			}
			return fixture.Draw(panel);
		}

		// Makes the Graph Editor window fill the fixture's display, so its side panel shows every row of a node's details.
		void EnlargeGraphWindow(Test::UtilityPanelFixture& fixture, GraphEditorPanel& panel)
		{
			ImGui::SetWindowPos("Graph Editor", ImVec2(0.0f, 0.0f));
			ImGui::SetWindowSize("Graph Editor", ImVec2(1200.0f, 900.0f));
			static_cast<void>(fixture.Draw(panel));
		}

		void MoveMouse(Test::UtilityPanelFixture& fixture, GraphEditorPanel& panel, const glm::vec2& point)
		{
			ImGui::GetIO().AddMousePosEvent(point.x, point.y);
			static_cast<void>(fixture.Draw(panel));
		}

		// A left-button drag from `from` to `to`, through their midpoint.
		void Drag(Test::UtilityPanelFixture& fixture, GraphEditorPanel& panel, const glm::vec2& from, const glm::vec2& to)
		{
			MoveMouse(fixture, panel, from);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			static_cast<void>(fixture.Draw(panel));
			MoveMouse(fixture, panel, (from + to) * 0.5f);
			MoveMouse(fixture, panel, to);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			static_cast<void>(fixture.Draw(panel));
		}

		void Click(Test::UtilityPanelFixture& fixture, GraphEditorPanel& panel, const glm::vec2& point, ImGuiMouseButton button = ImGuiMouseButton_Left)
		{
			MoveMouse(fixture, panel, point);
			ImGui::GetIO().AddMouseButtonEvent(button, true);
			static_cast<void>(fixture.Draw(panel));
			ImGui::GetIO().AddMouseButtonEvent(button, false);
			static_cast<void>(fixture.Draw(panel));
		}

		void PressChord(Test::UtilityPanelFixture& fixture, GraphEditorPanel& panel, ImGuiKey modifier, ImGuiKey key)
		{
			ImGui::GetIO().AddKeyEvent(modifier, true);
			fixture.Press(panel, key);
			ImGui::GetIO().AddKeyEvent(modifier, false);
			static_cast<void>(fixture.Draw(panel));
		}

		void TypeText(Test::UtilityPanelFixture& fixture, GraphEditorPanel& panel, std::string_view text)
		{
			ImGui::GetIO().AddInputCharactersUTF8(std::string(text).c_str());
			static_cast<void>(fixture.Draw(panel));
		}

		glm::vec2 PinPosition(const GraphEditorPanel& panel, uint32_t node, std::string_view pin)
		{
			const std::optional<glm::vec2> position = panel.GetPinScreenPosition(GraphPinRef{ .Node = node, .Pin = std::string(pin) });
			REQUIRE_MESSAGE(position.has_value(), pin);
			return *position;
		}

		// A point in node `node`'s title bar.
		glm::vec2 TitlePoint(const GraphEditorPanel& panel, uint32_t node)
		{
			const auto rect = panel.GetNodeScreenRect(node);
			REQUIRE(rect.has_value());
			return rect->first + glm::vec2(14.0f, 8.0f);
		}

		Json Link(uint32_t from, std::string_view fromPin, uint32_t to, std::string_view toPin)
		{
			return Json{ { "From", Json{ { "Node", from }, { "Pin", std::string(fromPin) } } }, { "To", Json{ { "Node", to }, { "Pin", std::string(toPin) } } } };
		}

		const ImGuiWindow* FindGraphWindow()
		{
			const ImGuiWindow* window = ImGui::FindWindowByName("Graph Editor");
			return window != nullptr && window->WasActive ? window : nullptr;
		}

		std::vector<uint32_t> Selected(const GraphEditorPanel& panel)
		{
			return std::vector<uint32_t>(panel.GetSelection().begin(), panel.GetSelection().end());
		}

		// The first line of the graph at DoorPath's transpiled text whose code is node `node`'s; 0 when none is.
		uint32_t FindNodeLine(Test::UtilityPanelFixture& fixture, uint32_t node)
		{
			const Json read = CallOrFail(fixture, "graph.read", Json{ { "graph", std::string(DoorPath) }, { "luau", true } });
			for (size_t index = 0; index < read["lineNodes"].size(); ++index)
				if (read["lineNodes"][index] == node)
					return static_cast<uint32_t>(index + 1);
			return 0;
		}

		// The ids graph.addNode gave a Counter's nodes.
		struct CounterNodes
		{
			uint32_t Step = 0;
			uint32_t Count = 0;
			uint32_t Increment = 0;
			uint32_t Add = 0;
			uint32_t Store = 0;
		};

		// The Counter built into the empty graph at `path`: each fixed step adds its Step field (2) to its Count variable.
		CounterNodes BuildCounter(Test::UtilityPanelFixture& fixture, std::string_view path)
		{
			const Json graph = std::string(path);
			const auto member = [](std::string_view name, double value, bool field)
			{
				Json described{ { "Name", std::string(name) }, { "Kind", "Number" }, { "Default", value }, { "Element", "Number" }, { "AssetType", "" },
					{ "EnumValues", Json::array() } };
				if (field)
					described["Options"] = Json::object();
				return described;
			};
			CallOrFail(fixture, "graph.setMembers", Json{ { "graph", graph }, { "fields", Json::array({ member("Step", 2, true) }) }, { "variables", Json::array({ member("Count", 0, false) }) } });
			const auto add = [&fixture, &graph](std::string_view kind, float x, float y)
			{
				const Json added = CallOrFail(fixture, "graph.addNode", Json{ { "graph", graph }, { "kind", std::string(kind) }, { "position", Json::array({ x, y }) } });
				return JsonReader(added["node"]).ReadUInt32().value_or(0);
			};
			const CounterNodes nodes{ .Step = add("Event.OnFixedUpdate", 0.0f, 0.0f),
				.Count = add("Self.Get.Count", 0.0f, 160.0f),
				.Increment = add("Self.Get.Step", 0.0f, 320.0f),
				.Add = add("Number.Add", 240.0f, 200.0f),
				.Store = add("Self.Set.Count", 480.0f, 0.0f) };
			const auto link = [&fixture, &graph](uint32_t from, std::string_view fromPin, uint32_t to, std::string_view toPin)
			{
				CallOrFail(fixture, "graph.connect", Json{ { "graph", graph }, { "from", Json{ { "Node", from }, { "Pin", std::string(fromPin) } } }, { "to", Json{ { "Node", to }, { "Pin", std::string(toPin) } } } });
			};
			link(nodes.Step, "Then", nodes.Store, "In");
			link(nodes.Count, "Value", nodes.Add, "A");
			link(nodes.Increment, "Value", nodes.Add, "B");
			link(nodes.Add, "Result", nodes.Store, "Value");
			return nodes;
		}

		Json ScriptSlot(std::string_view path)
		{
			return Json{ { "Script", std::string(path) }, { "Fields", Json::object() }, { "ExecutionOrder", 0 } };
		}

		GraphEditorTarget OpenTarget(AssetHandle graph, uint32_t node = 0, uint32_t line = 0)
		{
			return GraphEditorTarget{ .Graph = graph, .Node = node, .Line = line, .JsonPointer = {}, .Finding = {} };
		}

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("GraphEditorPanel: the window shows the open graph's nodes until it is closed")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			CHECK_FALSE(panel.GetGraph().IsValid());
			static_cast<void>(fixture.Draw(panel));
			CHECK(FindGraphWindow() == nullptr);

			const AssetHandle graph = OpenDoor(fixture, panel);
			const uint32_t start = AddNode(fixture, "Event.OnStart", 0.0f, 0.0f);
			const uint32_t log = AddNode(fixture, "Call.Log.Info", 240.0f, 0.0f);
			const std::string text = Settle(fixture, panel);
			CHECK(panel.GetGraph() == graph);
			CHECK(text.contains("Door"));
			REQUIRE(FindGraphWindow() != nullptr);
			// Both nodes are on the canvas, side by side as their positions place them.
			const auto first = panel.GetNodeScreenRect(start);
			const auto second = panel.GetNodeScreenRect(log);
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			CHECK(second->first.x - first->first.x == doctest::Approx(240.0f * panel.GetView().Zoom));
			CHECK(second->first.y == doctest::Approx(first->first.y));
			CHECK(panel.GetPinScreenPosition(GraphPinRef{ .Node = start, .Pin = "Then" }).has_value());
			CHECK_FALSE(panel.GetPinScreenPosition(GraphPinRef{ .Node = start, .Pin = "Nope" }).has_value());

			// An agent's edit shows at once.
			const uint32_t added = AddNode(fixture, "Flow.Branch", 0.0f, 200.0f);
			static_cast<void>(Settle(fixture, panel));
			CHECK(panel.GetNodeScreenRect(added).has_value());

			panel.Close();
			CHECK_FALSE(panel.GetGraph().IsValid());
			static_cast<void>(fixture.Draw(panel));
			static_cast<void>(fixture.Draw(panel));
			CHECK(FindGraphWindow() == nullptr);
			CHECK_FALSE(panel.GetNodeScreenRect(start).has_value());
		}

		TEST_CASE("GraphEditorPanel: the node menu adds the chosen kind where the canvas was right-clicked through graph.addNode")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			const uint32_t start = AddNode(fixture, "Event.OnStart", 0.0f, 0.0f);
			static_cast<void>(Settle(fixture, panel));
			const glm::vec2 empty = TitlePoint(panel, start) + glm::vec2(0.0f, 200.0f);
			const float zoom = panel.GetView().Zoom;
			Click(fixture, panel, empty, ImGuiMouseButton_Right);
			static_cast<void>(fixture.Draw(panel));
			TypeText(fixture, panel, "Flow.Branch");
			fixture.Press(panel, ImGuiKey_Enter);
			fixture.Pump();
			const Json document = ReadDocument(fixture);
			REQUIRE(document["Nodes"].size() == 2);
			CHECK(document["Nodes"][1]["Kind"] == "Flow.Branch");
			const glm::vec2 placed(JsonReader(document["Nodes"][1]["Position"][0]).ReadFloat().value_or(0.0f), JsonReader(document["Nodes"][1]["Position"][1]).ReadFloat().value_or(0.0f));
			CHECK(placed.x == doctest::Approx(14.0f / zoom).epsilon(0.05));
			CHECK(placed.y == doctest::Approx(208.0f / zoom).epsilon(0.05));
			// The user's edit, one undo step under the method's label.
			const std::vector<CommandHistoryEntry> entries = fixture.GetEditor().GetHistory().GetEntries(10);
			CHECK(std::ranges::any_of(entries, [](const CommandHistoryEntry& entry)
			{
				return entry.Label.contains("Add Graph Node") && entry.Origin == CommandOrigin::User;
			}));
		}

		TEST_CASE("GraphEditorPanel: a link dragged between two pins connects them through graph.connect, and Alt+click on it disconnects them")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			const uint32_t start = AddNode(fixture, "Event.OnStart", 0.0f, 0.0f);
			const uint32_t log = AddNode(fixture, "Call.Log.Info", 240.0f, 0.0f);
			static_cast<void>(Settle(fixture, panel));

			Drag(fixture, panel, PinPosition(panel, start, "Then"), PinPosition(panel, log, "In"));
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Links"] == Json::array({ Link(start, "Then", log, "In") }));
			static_cast<void>(Settle(fixture, panel));

			const auto curve = GetGraphLinkCurve(PinPosition(panel, start, "Then"), PinPosition(panel, log, "In"));
			const glm::vec2 middle = 0.125f * curve[0] + 0.375f * curve[1] + 0.375f * curve[2] + 0.125f * curve[3];
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
			Click(fixture, panel, middle);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Links"].empty());

			// A link dragged off its input and dropped on empty canvas goes too.
			Drag(fixture, panel, PinPosition(panel, start, "Then"), PinPosition(panel, log, "In"));
			fixture.Pump();
			static_cast<void>(Settle(fixture, panel));
			REQUIRE(ReadDocument(fixture)["Links"].size() == 1);
			Drag(fixture, panel, PinPosition(panel, log, "In"), PinPosition(panel, log, "In") + glm::vec2(0.0f, 160.0f));
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Links"].empty());
		}

		TEST_CASE("GraphEditorPanel: dragging a node moves it through graph.moveNodes, and Delete removes the selection through graph.removeNodes")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			const uint32_t start = AddNode(fixture, "Event.OnStart", 0.0f, 0.0f);
			const uint32_t log = AddNode(fixture, "Call.Log.Info", 240.0f, 0.0f);
			static_cast<void>(Settle(fixture, panel));
			REQUIRE(panel.GetView().Zoom == 1.0f);

			const glm::vec2 title = TitlePoint(panel, log);
			Drag(fixture, panel, title, title + glm::vec2(60.0f, 80.0f));
			CHECK(std::vector<uint32_t>(panel.GetSelection().begin(), panel.GetSelection().end()) == std::vector<uint32_t>{ log });
			fixture.Pump();
			Json document = ReadDocument(fixture);
			CHECK(document["Nodes"][1]["Position"] == Json::array({ 300, 80 }));
			CHECK(document["Nodes"][0]["Position"] == Json::array({ 0, 0 }));
			static_cast<void>(Settle(fixture, panel));

			fixture.Press(panel, ImGuiKey_Delete);
			fixture.Pump();
			document = ReadDocument(fixture);
			REQUIRE(document["Nodes"].size() == 1);
			CHECK(document["Nodes"][0]["Id"] == start);
		}

		TEST_CASE("GraphEditorPanel: Ctrl+D duplicates the box-selected nodes and the link between them in one undo step")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			const uint32_t start = AddNode(fixture, "Event.OnStart", 0.0f, 0.0f);
			const uint32_t log = AddNode(fixture, "Call.Log.Info", 240.0f, 0.0f);
			CallOrFail(fixture, "graph.connect", Json{ { "graph", std::string(DoorPath) }, { "from", Json{ { "Node", start }, { "Pin", "Then" } } }, { "to", Json{ { "Node", log }, { "Pin", "In" } } } });
			static_cast<void>(Settle(fixture, panel));

			// A box from above the first node's corner to below the second's selects both.
			const auto first = panel.GetNodeScreenRect(start);
			const auto second = panel.GetNodeScreenRect(log);
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			Drag(fixture, panel, first->first - glm::vec2(20.0f), second->second + glm::vec2(20.0f));
			CHECK(std::vector<uint32_t>(panel.GetSelection().begin(), panel.GetSelection().end()) == std::vector<uint32_t>{ start, log });

			const size_t steps = fixture.GetEditor().GetHistory().GetUndoCount();
			PressChord(fixture, panel, ImGuiMod_Ctrl, ImGuiKey_D);
			fixture.Pump();
			const Json document = ReadDocument(fixture);
			REQUIRE(document["Nodes"].size() == 4);
			CHECK(document["Nodes"][2]["Kind"] == "Event.OnStart");
			CHECK(document["Nodes"][3]["Kind"] == "Call.Log.Info");
			const uint32_t copiedStart = JsonReader(document["Nodes"][2]["Id"]).ReadUInt32().value_or(0);
			const uint32_t copiedLog = JsonReader(document["Nodes"][3]["Id"]).ReadUInt32().value_or(0);
			CHECK(document["Links"] == Json::array({ Link(start, "Then", log, "In"), Link(copiedStart, "Then", copiedLog, "In") }));
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() == steps + 1);
		}

		TEST_CASE("GraphEditorPanel: a link dropped on empty canvas adds the kind chosen from the linkable ones, linked, in one undo step")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			const uint32_t start = AddNode(fixture, "Event.OnStart", 0.0f, 0.0f);
			static_cast<void>(Settle(fixture, panel));
			const size_t steps = fixture.GetEditor().GetHistory().GetUndoCount();

			const glm::vec2 then = PinPosition(panel, start, "Then");
			Drag(fixture, panel, then, then + glm::vec2(160.0f, 120.0f));
			static_cast<void>(fixture.Draw(panel));
			TypeText(fixture, panel, "Call.Log.Info");
			fixture.Press(panel, ImGuiKey_Enter);
			fixture.Pump();
			const Json document = ReadDocument(fixture);
			REQUIRE(document["Nodes"].size() == 2);
			CHECK(document["Nodes"][1]["Kind"] == "Call.Log.Info");
			const uint32_t log = JsonReader(document["Nodes"][1]["Id"]).ReadUInt32().value_or(0);
			CHECK(document["Links"] == Json::array({ Link(start, "Then", log, "In") }));
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() == steps + 1);
		}

		TEST_CASE("GraphEditorPanel: an input's literal field submits graph.setValues and the members panel submits graph.setMembers, a field's options included")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			const uint32_t branch = AddNode(fixture, "Flow.Branch", 0.0f, 0.0f);
			static_cast<void>(Settle(fixture, panel));
			// The side panel lists the members (M39's opt-in and Methods section included) before the node's literals: room
			// for both.
			EnlargeGraphWindow(fixture, panel);
			Click(fixture, panel, TitlePoint(panel, branch));
			CHECK(std::vector<uint32_t>(panel.GetSelection().begin(), panel.GetSelection().end()) == std::vector<uint32_t>{ branch });
			static_cast<void>(fixture.Draw(panel));

			// The selected node's Bool input is a checkbox labelled with its pin and type.
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Condition (Bool)");
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Nodes"][0]["Values"] == Json{ { "Condition", true } });
			static_cast<void>(Settle(fixture, panel));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Clear literal");
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Nodes"][0]["Values"] == Json::object());

			// A new field: its name, then Add field (a Number by default).
			static_cast<void>(Settle(fixture, panel));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "New member name");
			TypeText(fixture, panel, "Speed");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Add field");
			fixture.Pump();
			const Json fields = ReadDocument(fixture)["Fields"];
			REQUIRE(fields.size() == 1);
			CHECK(fields[0]["Name"] == "Speed");
			CHECK(fields[0]["Kind"] == "Number");
			CHECK(fields[0]["Default"] == 0);

			// Its options: a bound added starts at the default, which it holds.
			static_cast<void>(Settle(fixture, panel));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Options");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Add Max");
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Fields"][0]["Options"] == Json{ { "Max", 0 } });
		}

		// M39 (Docs/Decisions/0056-m39-graphs-behaviours.md decision 13): the side panel's opt-in checkbox and Methods section
		// submit graph.setMembers, the method written whole each time (its parameters and its result); a name the graph holds
		// adds nothing, and the panel's own listing of the graph's kinds, which its node menu offers, follows the method.
		TEST_CASE("GraphEditorPanel: the members panel sets the world-pause opt-in and adds a method, its parameter and its result through graph.setMembers")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			static_cast<void>(Settle(fixture, panel));
			EnlargeGraphWindow(fixture, panel);

			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Runs while the world is paused");
			fixture.Pump();
			CHECK(ReadDocument(fixture)["RunsWhileWorldPaused"] == true);

			static_cast<void>(Settle(fixture, panel));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "New method name");
			TypeText(fixture, panel, "Pull");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Add method");
			fixture.Pump();
			Json methods = ReadDocument(fixture)["Methods"];
			REQUIRE(methods.size() == 1);
			CHECK(methods[0] == Json{ { "Name", "Pull" }, { "Parameters", Json::array() }, { "Results", Json::array() } });
			// The members edit kept the opt-in.
			CHECK(ReadDocument(fixture)["RunsWhileWorldPaused"] == true);

			static_cast<void>(Settle(fixture, panel));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Add parameter");
			fixture.Pump();
			methods = ReadDocument(fixture)["Methods"];
			REQUIRE(methods[0]["Parameters"].size() == 1);
			CHECK(methods[0]["Parameters"][0] == Json{ { "Name", "arg1" }, { "Kind", "Any" }, { "AssetType", "" } });
			CHECK(methods[0]["Results"].empty());

			// The result: Bool, chosen in the method's result combo ("No result" first), with the parameter kept.
			static_cast<void>(Settle(fixture, panel));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "No result");
			static_cast<void>(fixture.Draw(panel));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Bool");
			fixture.Pump();
			methods = ReadDocument(fixture)["Methods"];
			REQUIRE(methods.size() == 1);
			CHECK(methods[0]["Results"] == Json::array({ Json{ { "Kind", "Bool" }, { "AssetType", "" } } }));
			CHECK(methods[0]["Parameters"].size() == 1);

			// Pull again: Add method stays disabled, so the method keeps its parameter and result.
			static_cast<void>(Settle(fixture, panel));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "New method name");
			TypeText(fixture, panel, "Pull");
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Add method");
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Methods"] == methods);

			// The panel's own listing of the graph's kinds follows the method: its node menu adds a Method.Pull node.
			const uint32_t start = AddNode(fixture, "Event.OnStart", 0.0f, 0.0f);
			static_cast<void>(Settle(fixture, panel));
			Click(fixture, panel, TitlePoint(panel, start) + glm::vec2(0.0f, 200.0f), ImGuiMouseButton_Right);
			static_cast<void>(fixture.Draw(panel));
			TypeText(fixture, panel, "Method.Pull");
			fixture.Press(panel, ImGuiKey_Enter);
			fixture.Pump();
			const Json document = ReadDocument(fixture);
			REQUIRE(document["Nodes"].size() == 2);
			CHECK(document["Nodes"][1]["Kind"] == "Method.Pull");
		}

		TEST_CASE("GraphEditorPanel: Undo and Redo submit edit.undo and edit.redo")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			static_cast<void>(AddNode(fixture, "Event.OnStart", 0.0f, 0.0f));
			static_cast<void>(Settle(fixture, panel));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Undo");
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Nodes"].empty());
			static_cast<void>(Settle(fixture, panel));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Redo");
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Nodes"].size() == 1);
		}

		TEST_CASE("GraphEditorPanel: a target's line of the transpiled text selects and frames the node whose code it is")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			fixture.OpenProject();
			CallOrFail(fixture, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const uint32_t start = AddNode(fixture, "Event.OnStart", 0.0f, 0.0f);
			const uint32_t log = AddNode(fixture, "Call.Log.Info", 6000.0f, 4000.0f);
			CallOrFail(fixture, "graph.connect", Json{ { "graph", std::string(DoorPath) }, { "from", Json{ { "Node", start }, { "Pin", "Then" } } }, { "to", Json{ { "Node", log }, { "Pin", "In" } } } });
			const Json read = CallOrFail(fixture, "graph.read", Json{ { "graph", std::string(DoorPath) }, { "luau", true } });
			uint32_t line = 0;
			for (size_t index = 0; index < read["lineNodes"].size() && line == 0; ++index)
				if (read["lineNodes"][index] == log)
					line = static_cast<uint32_t>(index + 1);
			REQUIRE(line != 0);

			const std::optional<AssetHandle> graph = fixture.GetEditor().GetAssets().Resolve(DoorPath);
			REQUIRE(graph.has_value());
			panel.Open(GraphEditorTarget{ .Graph = *graph, .Node = 0, .Line = line, .JsonPointer = {}, .Finding = {} });
			static_cast<void>(Settle(fixture, panel));
			CHECK(std::vector<uint32_t>(panel.GetSelection().begin(), panel.GetSelection().end()) == std::vector<uint32_t>{ log });
			// Framed: the node is in the window, and the event node, far from it, is not.
			const ImGuiWindow* window = FindGraphWindow();
			REQUIRE(window != nullptr);
			const auto framed = panel.GetNodeScreenRect(log);
			const auto other = panel.GetNodeScreenRect(start);
			REQUIRE(framed.has_value());
			REQUIRE(other.has_value());
			const glm::vec2 center = (framed->first + framed->second) * 0.5f;
			CHECK(window->Rect().Contains(ImVec2(center.x, center.y)));
			CHECK_FALSE(window->Rect().Overlaps(ImRect(ImVec2(other->first.x, other->first.y), ImVec2(other->second.x, other->second.y))));
		}

		// W24.2's review: the usual debugging loop opens a finding of the graph already open, whose last read was asked without
		// the text's line map; the target resolves against the read it asks for, never an older one.
		TEST_CASE("GraphEditorPanel: a finding's line selects and frames its node in the graph already open, even while a read of it is in flight")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			const AssetHandle graph = OpenDoor(fixture, panel);
			const uint32_t start = AddNode(fixture, "Event.OnStart", 0.0f, 0.0f);
			const uint32_t log = AddNode(fixture, "Call.Log.Info", 6000.0f, 4000.0f);
			CallOrFail(fixture, "graph.connect", Json{ { "graph", std::string(DoorPath) }, { "from", Json{ { "Node", start }, { "Pin", "Then" } } }, { "to", Json{ { "Node", log }, { "Pin", "In" } } } });
			const uint32_t line = FindNodeLine(fixture, log);
			REQUIRE(line != 0);
			static_cast<void>(Settle(fixture, panel));
			REQUIRE(Selected(panel).empty());

			// Open, settled: the target waits for the read it asks for, which carries the line map.
			panel.Open(OpenTarget(graph, 0, line));
			static_cast<void>(Settle(fixture, panel));
			CHECK(Selected(panel) == std::vector<uint32_t>{ log });
			const ImGuiWindow* window = FindGraphWindow();
			REQUIRE(window != nullptr);
			const auto framed = panel.GetNodeScreenRect(log);
			REQUIRE(framed.has_value());
			const glm::vec2 center = (framed->first + framed->second) * 0.5f;
			CHECK(window->Rect().Contains(ImVec2(center.x, center.y)));

			// Open while a read is in flight: an agent's edit made the window read again, without the line map, just before.
			panel.Open(OpenTarget(graph, start));
			static_cast<void>(Settle(fixture, panel));
			REQUIRE(Selected(panel) == std::vector<uint32_t>{ start });
			CallOrFail(fixture, "graph.moveNodes", Json{ { "graph", std::string(DoorPath) }, { "positions", Json::array({ Json{ { "node", start }, { "position", Json::array({ 0, 80 }) } } }) } });
			static_cast<void>(fixture.Draw(panel));
			panel.Open(OpenTarget(graph, 0, line));
			static_cast<void>(Settle(fixture, panel));
			CHECK(Selected(panel) == std::vector<uint32_t>{ log });
		}

		// W24.2's review: a link carried off its input onto another moves in one edit.batch, so a refused connect undoes the
		// disconnect; every refusal is reported, the window showing the method's own error.
		TEST_CASE("GraphEditorPanel: a link dragged onto another input moves there in one undo step, and an edit the method refuses changes nothing and shows its error")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			const uint32_t start = AddNode(fixture, "Event.OnStart", 0.0f, 0.0f);
			const uint32_t add = AddNode(fixture, "Number.Add", 0.0f, 200.0f);
			const uint32_t first = AddNode(fixture, "Call.Log.Info", 320.0f, 0.0f);
			const uint32_t second = AddNode(fixture, "Call.Log.Info", 320.0f, 200.0f);
			const uint32_t branch = AddNode(fixture, "Flow.Branch", 320.0f, 400.0f);
			CallOrFail(fixture, "graph.connect", Json{ { "graph", std::string(DoorPath) }, { "from", Json{ { "Node", add }, { "Pin", "Result" } } }, { "to", Json{ { "Node", first }, { "Pin", "Values" } } } });
			static_cast<void>(Settle(fixture, panel));
			const CommandHistory& history = fixture.GetEditor().GetHistory();
			const size_t steps = history.GetUndoCount();

			Drag(fixture, panel, PinPosition(panel, first, "Values"), PinPosition(panel, second, "Values"));
			fixture.Pump();
			const Json moved = ReadDocument(fixture)["Links"];
			CHECK(moved == Json::array({ Link(add, "Result", second, "Values") }));
			CHECK(history.GetUndoCount() == steps + 1);
			static_cast<void>(Settle(fixture, panel));

			// Onto an input that cannot take it (a Number into a Bool): graph.connect refuses, the batch undoes its disconnect.
			{
				const Test::ExpectLog reported(LogLevel::Error, "Graph editor");
				Drag(fixture, panel, PinPosition(panel, second, "Values"), PinPosition(panel, branch, "Condition"));
				fixture.Pump();
				const std::string text = Settle(fixture, panel);
				CHECK(ReadDocument(fixture)["Links"] == moved);
				CHECK(history.GetUndoCount() == steps + 1);
				const Result<GraphEditorRequest> request = MakeGraphMoveLinkRequest(fixture.GetEditor().GetTypeRegistry(), DoorPath,
					GraphLink{ .From = { .Node = add, .Pin = "Result" }, .To = { .Node = second, .Pin = "Values" } }, GraphPinRef{ .Node = branch, .Pin = "Condition" });
				REQUIRE(request.has_value());
				const Result<Json> refused = fixture.GetClient().Call(request->Method, request->Params);
				REQUIRE_FALSE(refused.has_value());
				CHECK(text.contains(refused.error().GetMessageText()));
			}

			// A new link between pins that cannot link (a flow into a Number) is refused the same way.
			{
				const Test::ExpectLog reported(LogLevel::Error, "Graph editor");
				Drag(fixture, panel, PinPosition(panel, start, "Then"), PinPosition(panel, add, "A"));
				fixture.Pump();
				const std::string text = Settle(fixture, panel);
				CHECK(ReadDocument(fixture)["Links"] == moved);
				CHECK(history.GetUndoCount() == steps + 1);
				const Result<Json> refused = fixture.GetClient().Call("graph.connect",
					Json{ { "graph", std::string(DoorPath) }, { "from", Json{ { "Node", start }, { "Pin", "Then" } } }, { "to", Json{ { "Node", add }, { "Pin", "A" } } } });
				REQUIRE_FALSE(refused.has_value());
				CHECK(text.contains(refused.error().GetMessageText()));
			}
		}

		TEST_CASE("GraphEditorPanel: Ctrl+C and Ctrl+V paste the copied nodes and the link between them at the mouse in one undo step")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			const uint32_t start = AddNode(fixture, "Event.OnStart", 0.0f, 0.0f);
			const uint32_t log = AddNode(fixture, "Call.Log.Info", 240.0f, 0.0f);
			CallOrFail(fixture, "graph.connect", Json{ { "graph", std::string(DoorPath) }, { "from", Json{ { "Node", start }, { "Pin", "Then" } } }, { "to", Json{ { "Node", log }, { "Pin", "In" } } } });
			static_cast<void>(Settle(fixture, panel));
			REQUIRE(panel.GetView().Zoom == 1.0f);
			const auto first = panel.GetNodeScreenRect(start);
			const auto second = panel.GetNodeScreenRect(log);
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			Drag(fixture, panel, first->first - glm::vec2(20.0f), second->second + glm::vec2(20.0f));
			REQUIRE(Selected(panel) == std::vector<uint32_t>{ start, log });
			PressChord(fixture, panel, ImGuiMod_Ctrl, ImGuiKey_C);
			fixture.Pump();
			// Copying is presentation: nothing is submitted.
			const size_t steps = fixture.GetEditor().GetHistory().GetUndoCount();
			CHECK(ReadDocument(fixture)["Nodes"].size() == 2);

			// The mouse over empty canvas below the nodes: the first copied node goes under it, the rest beside it as copied.
			MoveMouse(fixture, panel, first->first + glm::vec2(0.0f, 150.0f));
			PressChord(fixture, panel, ImGuiMod_Ctrl, ImGuiKey_V);
			fixture.Pump();
			const Json document = ReadDocument(fixture);
			REQUIRE(document["Nodes"].size() == 4);
			CHECK(document["Nodes"][2]["Kind"] == "Event.OnStart");
			CHECK(document["Nodes"][2]["Position"] == Json::array({ 0, 150 }));
			CHECK(document["Nodes"][3]["Kind"] == "Call.Log.Info");
			CHECK(document["Nodes"][3]["Position"] == Json::array({ 240, 150 }));
			const uint32_t pastedStart = JsonReader(document["Nodes"][2]["Id"]).ReadUInt32().value_or(0);
			const uint32_t pastedLog = JsonReader(document["Nodes"][3]["Id"]).ReadUInt32().value_or(0);
			CHECK(document["Links"] == Json::array({ Link(start, "Then", log, "In"), Link(pastedStart, "Then", pastedLog, "In") }));
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() == steps + 1);
		}

		// W24.2's review: a number field writes what a .graph file holds for its float (GraphDocument.h's number rule), so the
		// method never refuses the user's number.
		TEST_CASE("GraphEditorPanel: a number literal typed into its field is written as the float's shortest spelling")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			const uint32_t add = AddNode(fixture, "Number.Add", 0.0f, 0.0f);
			static_cast<void>(Settle(fixture, panel));
			Click(fixture, panel, TitlePoint(panel, add));
			REQUIRE(Selected(panel) == std::vector<uint32_t>{ add });
			static_cast<void>(fixture.Draw(panel));

			// Ctrl+click on the A field's drag turns it into a text field (the label is drawn right of the field).
			const std::optional<ImVec2> label = Test::SupportingPanelTestUi::FindText(fixture, panel, "A (Number)");
			REQUIRE(label.has_value());
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			fixture.ClickAt(panel, label->x - 60.0f, label->y);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			static_cast<void>(fixture.Draw(panel));
			TypeText(fixture, panel, "0.1");
			fixture.Press(panel, ImGuiKey_Enter);
			fixture.Pump();
			// The float 0.1f, whose double is 0.100000001490116..., is written as 0.1.
			CHECK(ReadDocument(fixture)["Nodes"][0]["Values"] == Json{ { "A", 0.1 } });
		}

		// M24's review: a read that arrives while a literal's field is being edited (another edit's, an agent's, a reimport's)
		// applies to the rest of the window and leaves the field's drag or typed text, which its release still submits whole.
		TEST_CASE("GraphEditorPanel: a field being edited keeps its drag or text across a read that arrives meanwhile, and its release submits it")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			const uint32_t add = AddNode(fixture, "Number.Add", 0.0f, 0.0f);
			const uint32_t log = AddNode(fixture, "Call.Log.Info", 0.0f, 240.0f);
			static_cast<void>(Settle(fixture, panel));
			EnlargeGraphWindow(fixture, panel);
			// An agent's edit meanwhile: the window reads the graph again, and shows the move, while the field stays active.
			const auto readDuringEdit = [&fixture, &panel](uint32_t node, float y)
			{
				const auto before = panel.GetNodeScreenRect(node);
				REQUIRE(before.has_value());
				CallOrFail(fixture, "graph.moveNodes", Json{ { "graph", std::string(DoorPath) }, { "positions", Json::array({ Json{ { "node", node }, { "position", Json::array({ 0, y }) } } }) } });
				for (int round = 0; round < 6; ++round)
				{
					static_cast<void>(fixture.Draw(panel));
					fixture.Pump();
				}
				const auto after = panel.GetNodeScreenRect(node);
				REQUIRE(after.has_value());
				CHECK(after->first.y != doctest::Approx(before->first.y));
			};

			// Number.Add's A (a drag, 0.05 a pixel, its label drawn right of it), dragged 100 pixels, then a read, then 20 more.
			Click(fixture, panel, TitlePoint(panel, add));
			REQUIRE(Selected(panel) == std::vector<uint32_t>{ add });
			static_cast<void>(fixture.Draw(panel));
			const std::optional<ImVec2> number = Test::SupportingPanelTestUi::FindText(fixture, panel, "A (Number)");
			REQUIRE(number.has_value());
			const glm::vec2 grab(number->x - 60.0f, number->y);
			MoveMouse(fixture, panel, grab);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			static_cast<void>(fixture.Draw(panel));
			for (int step = 1; step <= 10; ++step)
				MoveMouse(fixture, panel, grab + glm::vec2(10.0f * static_cast<float>(step), 0.0f));
			readDuringEdit(add, 80.0f);
			CHECK(ReadDocument(fixture)["Nodes"][0]["Values"] == Json::object());
			for (int step = 1; step <= 2; ++step)
				MoveMouse(fixture, panel, grab + glm::vec2(100.0f + 10.0f * static_cast<float>(step), 0.0f));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			static_cast<void>(fixture.Draw(panel));
			fixture.Pump();
			// The whole drag, about 6, not the 1 dragged after the read.
			const double dragged = JsonReader(ReadDocument(fixture)["Nodes"][0]["Values"]["A"]).ReadDouble().value_or(0.0);
			CHECK(dragged > 4.5);
			static_cast<void>(Settle(fixture, panel));

			// Log.Info's Values (an optional Any, a text field showing its stored null): "Door" typed, a read, then Enter.
			Click(fixture, panel, TitlePoint(panel, log));
			REQUIRE(Selected(panel) == std::vector<uint32_t>{ log });
			static_cast<void>(fixture.Draw(panel));
			const std::optional<ImVec2> text = Test::SupportingPanelTestUi::FindText(fixture, panel, "Values (");
			REQUIRE(text.has_value());
			fixture.ClickAt(panel, text->x - 60.0f, text->y);
			PressChord(fixture, panel, ImGuiMod_Ctrl, ImGuiKey_A);
			TypeText(fixture, panel, "Door");
			readDuringEdit(log, 320.0f);
			CHECK(ReadDocument(fixture)["Nodes"][1]["Values"] == Json::object());
			fixture.Press(panel, ImGuiKey_Enter);
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Nodes"][1]["Values"] == Json{ { "Values", "Door" } });
		}

		// M24's review: a link or literal on a pin the node's kind no longer has (a binding's parameter renamed or removed,
		// decision 10) shows as a pin of its own marked as an error, its tooltip saying why, and goes through its method: the
		// link by Alt+click (graph.disconnect), the literal by the side panel's Clear literal (graph.setValues with null).
		TEST_CASE("GraphEditorPanel: a link or literal on a pin its node's kind no longer has shows as an error pin and is removed through its method")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			// Log.Info (node 2) has no "volume" input, which node 3's Result is linked to, and no "clip" input, which holds a literal.
			GraphDocument document;
			document.Name = "Door";
			document.Nodes = { GraphNode{ .Id = 1, .Kind = "Event.OnStart", .Position = glm::vec2(0.0f, 0.0f), .Values = {} },
				GraphNode{ .Id = 2, .Kind = "Call.Log.Info", .Position = glm::vec2(320.0f, 0.0f), .Values = { { "clip", VariantValue(Json("Assets/Old.wav")) } } },
				GraphNode{ .Id = 3, .Kind = "Number.Add", .Position = glm::vec2(0.0f, 200.0f), .Values = {} } };
			document.Links = { GraphLink{ .From = { 1, "Then" }, .To = { 2, "In" } }, GraphLink{ .From = { 3, "Result" }, .To = { 2, "volume" } } };
			REQUIRE(ValidateGraphStructure(document).has_value());
			const Result<std::string> text = GraphToText(document, fixture.GetEditor().GetTypeRegistry());
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const VfsPath path = VfsPath::Create("project", DoorPath).value_or(VfsPath());
			REQUIRE(fixture.GetEditor().WriteProjectFile(path, std::as_bytes(std::span(*text))).has_value());
			static_cast<void>(Settle(fixture, panel));
			EnlargeGraphWindow(fixture, panel);

			// Both stale pins are on the canvas; the reason shows in a stale pin's tooltip.
			const glm::vec2 volume = PinPosition(panel, 2, "volume");
			CHECK(panel.GetPinScreenPosition(GraphPinRef{ .Node = 2, .Pin = "clip" }).has_value());
			MoveMouse(fixture, panel, volume);
			CHECK(fixture.Draw(panel).contains("has no input 'volume' now"));

			// Alt+click on the stale link removes it through graph.disconnect.
			const auto curve = GetGraphLinkCurve(PinPosition(panel, 3, "Result"), volume);
			const glm::vec2 middle = 0.125f * curve[0] + 0.375f * curve[1] + 0.375f * curve[2] + 0.125f * curve[3];
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
			Click(fixture, panel, middle);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Links"] == Json::array({ Link(1, "Then", 2, "In") }));

			// The side panel lists the stale literal; its Clear literal removes it through graph.setValues with null.
			static_cast<void>(Settle(fixture, panel));
			CHECK_FALSE(panel.GetPinScreenPosition(GraphPinRef{ .Node = 2, .Pin = "volume" }).has_value());
			Click(fixture, panel, TitlePoint(panel, 2));
			REQUIRE(Selected(panel) == std::vector<uint32_t>{ 2 });
			CHECK(fixture.Draw(panel).contains("has no input clip now"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Clear literal");
			fixture.Pump();
			CHECK(ReadDocument(fixture)["Nodes"][1]["Values"] == Json::object());
			static_cast<void>(Settle(fixture, panel));
			CHECK_FALSE(panel.GetPinScreenPosition(GraphPinRef{ .Node = 2, .Pin = "clip" }).has_value());
		}

		// W24.2's integration (decision 14): stream B's live values in stream A's window.
		TEST_CASE("GraphEditorPanel: during play the canvas shows each node's last values and the side panel its members' values, read through graph.liveValues")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			// Door: each fixed step adds its Step field (2) to its Count variable.
			const Json graph = std::string(DoorPath);
			const auto member = [](std::string_view name, double value, bool field)
			{
				Json described{ { "Name", std::string(name) }, { "Kind", "Number" }, { "Default", value }, { "Element", "Number" }, { "AssetType", "" },
					{ "EnumValues", Json::array() } };
				if (field)
					described["Options"] = Json::object();
				return described;
			};
			CallOrFail(fixture, "graph.setMembers", Json{ { "graph", graph }, { "fields", Json::array({ member("Step", 2, true) }) }, { "variables", Json::array({ member("Count", 0, false) }) } });
			const uint32_t step = AddNode(fixture, "Event.OnFixedUpdate", 0.0f, 0.0f);
			const uint32_t count = AddNode(fixture, "Self.Get.Count", 0.0f, 160.0f);
			const uint32_t increment = AddNode(fixture, "Self.Get.Step", 0.0f, 320.0f);
			const uint32_t add = AddNode(fixture, "Number.Add", 240.0f, 200.0f);
			const uint32_t store = AddNode(fixture, "Self.Set.Count", 480.0f, 0.0f);
			const auto link = [&fixture, &graph](uint32_t from, std::string_view fromPin, uint32_t to, std::string_view toPin)
			{
				CallOrFail(fixture, "graph.connect", Json{ { "graph", graph }, { "from", Json{ { "Node", from }, { "Pin", std::string(fromPin) } } }, { "to", Json{ { "Node", to }, { "Pin", std::string(toPin) } } } });
			};
			link(step, "Then", store, "In");
			link(count, "Value", add, "A");
			link(increment, "Value", add, "B");
			link(add, "Result", store, "Value");
			const Json slot{ { "Script", std::string(DoorPath) }, { "Fields", Json::object() }, { "ExecutionOrder", 0 } };
			CallOrFail(fixture, "entity.create", Json{ { "name", "Door" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ slot }) } } } } } });
			std::string text = Settle(fixture, panel);
			// In edit mode nothing is read.
			CHECK_FALSE(panel.GetLiveView().GetTarget().has_value());
			CHECK(panel.GetLiveView().GetNodeLines(add).empty());
			CHECK_FALSE(text.contains("Count = "));

			CallOrFail(fixture, "play.start", Json{ { "lockstep", true }, { "seed", 5 } });
			CallOrFail(fixture, "play.step", Json{ { "ticks", 3 } });
			text = Settle(fixture, panel);
			REQUIRE_MESSAGE(panel.GetLiveView().GetValues().has_value(), panel.GetLiveView().GetError());
			CHECK(panel.GetLiveView().GetValues()->Tick == 3);
			// Under the nodes: each output's last value; the event's flow output holds none.
			CHECK(panel.GetLiveView().GetNodeLines(add) == std::vector<std::string>{ "Result = 6" });
			CHECK(panel.GetLiveView().GetNodeLines(count) == std::vector<std::string>{ "Value = 4" });
			CHECK(panel.GetLiveView().GetNodeLines(increment) == std::vector<std::string>{ "Value = 2" });
			CHECK(panel.GetLiveView().GetNodeLines(step).empty());
			// Beside the members: the instance's values; the toolbar names the instance and its tick.
			CHECK(text.contains("Count = 6"));
			CHECK(text.contains("Step = 2"));
			CHECK(text.contains("Live values of Door at tick 3"));

			// The values follow the session; a stopped session ends them.
			CallOrFail(fixture, "play.step", Json{ { "ticks", 1 } });
			text = Settle(fixture, panel);
			CHECK(panel.GetLiveView().GetNodeLines(add) == std::vector<std::string>{ "Result = 8" });
			CHECK(text.contains("Count = 8"));
			CallOrFail(fixture, "play.stop", Json::object());
			text = Settle(fixture, panel);
			CHECK_FALSE(panel.GetLiveView().GetValues().has_value());
			CHECK_FALSE(text.contains("Count = 8"));
			CHECK_FALSE(text.contains("Live values of"));

			// Closing the window stops the polling.
			CallOrFail(fixture, "play.start", Json{ { "lockstep", true }, { "seed", 5 } });
			static_cast<void>(Settle(fixture, panel));
			REQUIRE(panel.GetLiveView().GetTarget().has_value());
			panel.Close();
			static_cast<void>(Settle(fixture, panel));
			CHECK_FALSE(panel.GetLiveView().GetTarget().has_value());
			CallOrFail(fixture, "play.stop", Json::object());
		}

		// W24.2's review (decision 14): a node whose code raised a retained script error shows it, outlined as an error.
		TEST_CASE("GraphEditorPanel: during play a node whose code raised an error shows the error under it and is outlined as an error")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			static_cast<void>(OpenDoor(fixture, panel));
			// Door tags its Target field each fixed step; nothing sets it, so its first step raises at the AddTag node.
			const Json graph = std::string(DoorPath);
			CallOrFail(fixture, "graph.setMembers", Json{ { "graph", graph }, { "fields", Json::array({ Json{ { "Name", "Target" }, { "Kind", "Entity" }, { "Default", nullptr }, { "Element", "Number" }, { "AssetType", "" }, { "EnumValues", Json::array() }, { "Options", Json::object() } } }) } });
			const uint32_t tick = AddNode(fixture, "Event.OnFixedUpdate", 0.0f, 0.0f);
			const uint32_t target = AddNode(fixture, "Self.Get.Target", 0.0f, 160.0f);
			const uint32_t tag = JsonReader(CallOrFail(fixture, "graph.addNode", Json{ { "graph", graph }, { "kind", "Call.Entity.AddTag" }, { "position", Json::array({ 240, 0 }) }, { "values", Json{ { "tag", "Seen" } } } })["node"])
									 .ReadUInt32()
									 .value_or(0);
			CallOrFail(fixture, "graph.connect", Json{ { "graph", graph }, { "from", Json{ { "Node", tick }, { "Pin", "Then" } } }, { "to", Json{ { "Node", tag }, { "Pin", "In" } } } });
			CallOrFail(fixture, "graph.connect", Json{ { "graph", graph }, { "from", Json{ { "Node", target }, { "Pin", "Value" } } }, { "to", Json{ { "Node", tag }, { "Pin", "self" } } } });
			CallOrFail(fixture, "entity.create", Json{ { "name", "Door" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ ScriptSlot(DoorPath) }) } } } } } });
			static_cast<void>(Settle(fixture, panel));
			CHECK(panel.GetNodeOutline(tag) == GraphEditorPanel::NodeOutline::None);

			{
				const Test::ExpectLog raised(LogLevel::Error, "attempt to index nil with 'AddTag'");
				CallOrFail(fixture, "play.start", Json{ { "lockstep", true }, { "seed", 5 } });
				CallOrFail(fixture, "play.step", Json{ { "ticks", 2 } });
			}
			static_cast<void>(Settle(fixture, panel));
			// The AddTag node computes no value: its one line is the error, and its outline the error's.
			const std::vector<std::string> lines = panel.GetNodeLiveLines(tag);
			REQUIRE(lines.size() == 1);
			CHECK(lines.front().starts_with("Error: "));
			CHECK(panel.GetNodeOutline(tag) == GraphEditorPanel::NodeOutline::Error);
			// Nodes that raised nothing keep their outline and show no error.
			CHECK(panel.GetNodeLiveLines(target).empty());
			CHECK(panel.GetNodeOutline(target) == GraphEditorPanel::NodeOutline::None);
			CHECK(panel.GetNodeOutline(tick) == GraphEditorPanel::NodeOutline::None);
			// Selected, the node is outlined as the selection.
			Click(fixture, panel, TitlePoint(panel, tag));
			CHECK(panel.GetNodeOutline(tag) == GraphEditorPanel::NodeOutline::Selected);
			CallOrFail(fixture, "play.stop", Json::object());
			static_cast<void>(Settle(fixture, panel));
			CHECK(panel.GetNodeLiveLines(tag).empty());
		}

		// W24.2's review: graph.liveValues names an entity's slot by its behaviour name and reads the first slot of that name
		// (decision 14), which may hold another graph of that name; the canvas shows none of its values and says why.
		TEST_CASE("GraphEditorPanel: the canvas shows no live values of another graph that an earlier slot of the entity holds under the same behaviour name")
		{
			Test::UtilityPanelFixture fixture;
			GraphEditorPanel panel;
			fixture.OpenProject();
			constexpr std::string_view CopyPath = "Assets/Scripts/DoorCopy.graph";
			CallOrFail(fixture, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const CounterNodes door = BuildCounter(fixture, DoorPath);
			CallOrFail(fixture, "graph.create", Json{ { "path", std::string(CopyPath) }, { "name", "Door" } });
			const CounterNodes copy = BuildCounter(fixture, CopyPath);
			REQUIRE(copy.Add == door.Add); // node ids start at 1 in every graph
			CallOrFail(fixture, "entity.create", Json{ { "name", "Door" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ ScriptSlot(DoorPath), ScriptSlot(CopyPath) }) } } } } } });
			const std::optional<AssetHandle> doorGraph = fixture.GetEditor().GetAssets().Resolve(DoorPath);
			const std::optional<AssetHandle> copyGraph = fixture.GetEditor().GetAssets().Resolve(CopyPath);
			REQUIRE(doorGraph.has_value());
			REQUIRE(copyGraph.has_value());

			panel.Open(OpenTarget(*copyGraph));
			CallOrFail(fixture, "play.start", Json{ { "lockstep", true }, { "seed", 5 } });
			CallOrFail(fixture, "play.step", Json{ { "ticks", 3 } });
			std::string text = Settle(fixture, panel);
			// The read is of slot 0, Door's graph: nothing of it shows on the copy's nodes or members, and the window names it.
			REQUIRE_MESSAGE(panel.GetLiveView().GetValues().has_value(), panel.GetLiveView().GetError());
			CHECK(panel.GetLiveView().GetValues()->Slot == 0);
			CHECK(panel.GetLiveView().GetValues()->Graph.Path == DoorPath);
			CHECK(panel.GetNodeLiveLines(copy.Add).empty());
			CHECK(panel.GetNodeLiveLines(copy.Count).empty());
			CHECK_FALSE(text.contains("Count = "));
			CHECK_FALSE(text.contains("Live values of"));
			CHECK(text.contains(DoorPath));

			// Door's own graph shows them.
			panel.Open(OpenTarget(*doorGraph));
			text = Settle(fixture, panel);
			CHECK(panel.GetNodeLiveLines(door.Add) == std::vector<std::string>{ "Result = 6" });
			CHECK(text.contains("Count = 6"));
			CHECK(text.contains("Live values of Door at tick 3"));
			CallOrFail(fixture, "play.stop", Json::object());
		}

		// W24.2's review: graph.read, graph.nodes and graph.liveValues are among the reads a test run admits, so a graph opened
		// during a test run shows itself and the test session's instance.
		TEST_CASE("GraphEditorPanel: a graph opened while a test run owns the editor shows its nodes and the test session's live values")
		{
			// The stepped server clock runs one step of the pending test run a server pump, whatever the build's speed.
			Test::UtilityPanelFixture fixture(Test::UtilityPanelClock::Stepped);
			GraphEditorPanel panel;
			fixture.OpenProject();
			CallOrFail(fixture, "graph.create", Json{ { "path", std::string(DoorPath) } });
			const CounterNodes nodes = BuildCounter(fixture, DoorPath);
			CallOrFail(fixture, "entity.create", Json{ { "name", "Door" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ ScriptSlot(DoorPath) }) } } } } } });
			CallOrFail(fixture, "scene.save", Json::object());
			CallOrFail(fixture, "script.write", Json{ { "path", "Assets/Tests/Live.test.luau" }, { "source", "return Test.Suite('Live', function() Test.Case('long', function() Test.WaitTicks(1000) end) end)" } });
			CallOrFail(fixture, "project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Live.test.luau" }, { "Scene", "Assets/Scenes/Main.scene" }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } });
			const std::optional<AssetHandle> graph = fixture.GetEditor().GetAssets().Resolve(DoorPath);
			REQUIRE(graph.has_value());
			auto& client = fixture.GetClient();
			static_cast<void>(client.Submit("test.run", Json::object()));
			for (int pump = 0; pump < 4; ++pump)
				client.GetServer().Pump();
			REQUIRE(fixture.GetEditor().GetPlay().IsTestRunActive());

			panel.Open(OpenTarget(*graph));
			const std::string text = Settle(fixture, panel);
			REQUIRE(fixture.GetEditor().GetPlay().IsTestRunActive());
			CHECK(text.contains("Door  (Assets/Scripts/Door.graph)"));
			CHECK(panel.GetNodeScreenRect(nodes.Add).has_value());
			// The test session's instance: each fixed step adds Step (2) to Count.
			REQUIRE_MESSAGE(panel.GetLiveView().GetValues().has_value(), panel.GetLiveView().GetError());
			const uint64_t tick = panel.GetLiveView().GetValues()->Tick;
			REQUIRE(tick > 0);
			CHECK(panel.GetNodeLiveLines(nodes.Add) == std::vector<std::string>{ std::format("Result = {}", 2 * tick) });
			client.GetServer().DisconnectInProcess(client.GetClient());
			CHECK_FALSE(fixture.GetEditor().GetPlay().IsTestRunActive());
		}
	}

}
