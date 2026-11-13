#include "TestsPCH.h"

#include "EditorCore/Graph/GraphLiveView.h"

#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The graph editor's live values (M24, C-VS; Docs/Decisions/0035-m24-contract.md decision 14; W24.2's stream B): the polling
// the canvas and the inspector call once a frame, which reads graph.liveValues through the editor's actions between frames,
// and the text they draw.

namespace Engine {

	namespace {

		constexpr std::string_view CounterPath = "Assets/Scripts/Counter.graph";

		constexpr std::string_view TaggerPath = "Assets/Scripts/Tagger.graph";

		// The ids graph.addNode gave the Counter's nodes; the mover's are 0 without them.
		struct CounterNodes
		{
			uint32_t Step = 0;
			uint32_t Count = 0;
			uint32_t Increment = 0;
			uint32_t Add = 0;
			uint32_t Store = 0;
			uint32_t Counted = 0; // the Count the mover reads after the store
			uint32_t Place = 0;   // Vector.Make (Count, 0, 0)
		};

	}

	// `TClient`: a Test::AutomationFixture or a Test::AutomationTestClient.
	template<typename TClient>
	static Json CallOrFail(TClient& setup, std::string_view method, const Json& params)
	{
		Result<Json> result = setup.Call(method, params);
		INFO(std::string(method));
		REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
		return std::move(*result);
	}

	static Json NumberMember(std::string_view name, double value, bool field)
	{
		Json member{ { "Name", std::string(name) }, { "Kind", "Number" }, { "Default", value }, { "Element", "Number" }, { "AssetType", "" },
			{ "EnumValues", Json::array() } };
		if (field)
			member["Options"] = Json::object();
		return member;
	}

	static Json Pin(uint32_t node, std::string_view pin)
	{
		return Json{ { "Node", node }, { "Pin", std::string(pin) } };
	}

	// The Counter behaviour, built through the graph methods: each fixed step adds its Step field (1) to its Count variable, and,
	// with `moves`, then moves its entity to x = Count, so the state hash, which covers the Transform but no script table, sees
	// every Count (W24.2's review).
	template<typename TClient>
	static CounterNodes BuildCounter(TClient& setup, bool moves = false)
	{
		const Json graph = std::string(CounterPath);
		CallOrFail(setup, "graph.create", Json{ { "path", graph } });
		CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "fields", Json::array({ NumberMember("Step", 1, true) }) }, { "variables", Json::array({ NumberMember("Count", 0, false) }) } });
		const auto add = [&setup, &graph](std::string_view kind)
		{
			const Json added = CallOrFail(setup, "graph.addNode", Json{ { "graph", graph }, { "kind", std::string(kind) } });
			return JsonReader(added["node"]).ReadUInt32().value_or(0);
		};
		CounterNodes nodes{ .Step = add("Event.OnFixedUpdate"), .Count = add("Self.Get.Count"), .Increment = add("Self.Get.Step"), .Add = add("Number.Add"), .Store = add("Self.Set.Count") };
		const auto link = [&setup, &graph](uint32_t from, std::string_view fromPin, uint32_t to, std::string_view toPin)
		{
			CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(from, fromPin) }, { "to", Pin(to, toPin) } });
		};
		link(nodes.Step, "Then", nodes.Store, "In");
		link(nodes.Count, "Value", nodes.Add, "A");
		link(nodes.Increment, "Value", nodes.Add, "B");
		link(nodes.Add, "Result", nodes.Store, "Value");
		if (!moves)
			return nodes;
		const uint32_t self = add("Self.Entity");
		const uint32_t transform = add("Get.Entity.Transform");
		nodes.Counted = add("Self.Get.Count");
		nodes.Place = JsonReader(CallOrFail(setup, "graph.addNode", Json{ { "graph", graph }, { "kind", "Vector.Make" }, { "values", Json{ { "Y", 0 }, { "Z", 0 } } } })["node"])
						  .ReadUInt32()
						  .value_or(0);
		const uint32_t translate = add("Set.Transform.Translation");
		link(nodes.Store, "Then", translate, "In");
		link(self, "Value", transform, "self");
		link(transform, "Value", translate, "self");
		link(nodes.Counted, "Value", nodes.Place, "X");
		link(nodes.Place, "Result", translate, "Value");
		return nodes;
	}

	// The Tagger behaviour: each fixed step tags its Target field, which nothing sets, so its first step raises at the AddTag
	// node. Returns {the Self.Get.Target node, the AddTag node}.
	template<typename TClient>
	static std::pair<uint32_t, uint32_t> BuildTagger(TClient& setup)
	{
		const Json graph = std::string(TaggerPath);
		CallOrFail(setup, "graph.create", Json{ { "path", graph } });
		Json target{ { "Name", "Target" }, { "Kind", "Entity" }, { "Default", nullptr }, { "Element", "Number" }, { "AssetType", "" }, { "EnumValues", Json::array() },
			{ "Options", Json::object() } };
		CallOrFail(setup, "graph.setMembers", Json{ { "graph", graph }, { "fields", Json::array({ std::move(target) }) } });
		const auto add = [&setup, &graph](std::string_view kind, const Json& values)
		{
			const Json added = CallOrFail(setup, "graph.addNode", Json{ { "graph", graph }, { "kind", std::string(kind) }, { "values", values } });
			return JsonReader(added["node"]).ReadUInt32().value_or(0);
		};
		const uint32_t tick = add("Event.OnFixedUpdate", Json::object());
		const uint32_t read = add("Self.Get.Target", Json::object());
		const uint32_t tag = add("Call.Entity.AddTag", Json{ { "tag", "Seen" } });
		CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(tick, "Then") }, { "to", Pin(tag, "In") } });
		CallOrFail(setup, "graph.connect", Json{ { "graph", graph }, { "from", Pin(read, "Value") }, { "to", Pin(tag, "self") } });
		return { read, tag };
	}

	// An entity named `name` whose one script slot holds the Counter; returns its id.
	template<typename TClient>
	static UUID CreateCounterEntity(TClient& setup, std::string_view name)
	{
		const Json slot{ { "Script", std::string(CounterPath) }, { "Fields", Json::object() }, { "ExecutionOrder", 0 } };
		const Json created = CallOrFail(setup, "entity.create", Json{ { "name", std::string(name) }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ slot }) } } } } } });
		return JsonReader(created["entity"]["id"]).ReadUUID().value_or(UUID());
	}

	template<typename TClient>
	static AssetHandle FindCounterHandle(TClient& setup)
	{
		const Json info = CallOrFail(setup, "asset.info", Json{ { "asset", std::string(CounterPath) } });
		return JsonReader(info["asset"]["id"]).ReadUUID().value_or(UUID());
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("GraphLiveView: a live value is one line of text, a vector by its components and a value without JSON by its type")
		{
			CHECK(FormatGraphLiveValue("number", VariantValue(Json(2))) == "2");
			CHECK(FormatGraphLiveValue("number", VariantValue(Json(0.25))) == "0.25");
			CHECK(FormatGraphLiveValue("boolean", VariantValue(Json(true))) == "true");
			CHECK(FormatGraphLiveValue("string", VariantValue(Json("door"))) == "\"door\"");
			CHECK(FormatGraphLiveValue("vector", VariantValue(Json::array({ 1, 0.5, -2 }))) == "(1, 0.5, -2)");
			CHECK(FormatGraphLiveValue("Color", VariantValue(Json::array({ 1, 0.5, 0, 1 }))) == "(1, 0.5, 0, 1)");
			CHECK(FormatGraphLiveValue("Entity", VariantValue(Json("00000000000000a1"))) == "00000000000000a1");
			CHECK(FormatGraphLiveValue("table", VariantValue()) == "table");
			CHECK(FormatGraphLiveValue("Transform", VariantValue()) == "Transform");
			CHECK(FormatGraphLiveValue("number", VariantValue()) == "number");

			// A long string is cut after MaxGraphLiveTextBytes bytes, never inside a UTF-8 sequence.
			const std::string longText = std::string(MaxGraphLiveTextBytes - 1, 'a') + "\xC3\xA9tail";
			CHECK(FormatGraphLiveValue("string", VariantValue(Json(longText))) == "\"" + std::string(MaxGraphLiveTextBytes - 1, 'a') + "...\"");
			CHECK(GraphLiveView::FormatMemberLine({ .Name = "Count", .Type = "number", .Value = VariantValue(Json(3)) }) == "Count = 3");
			CHECK(GraphLiveView::FormatMemberLine({ .Name = "Target", .Type = "nil", .Value = {} }) == "Target = nil");
		}

		TEST_CASE("GraphLiveView: the canvas polls graph.liveValues once a frame between frames, for the selected or the first instance of its graph")
		{
			Test::AutomationFixture setup("GraphLiveView");
			const CounterNodes nodes = BuildCounter(setup);
			const UUID first = CreateCounterEntity(setup, "First");
			const UUID second = CreateCounterEntity(setup, "Second");
			const AssetHandle graph = FindCounterHandle(setup);
			REQUIRE(graph.IsValid());
			EditorContext& editor = setup.GetEditor();
			EditorActions actions(editor, setup.GetClient().GetServer());
			GraphLiveView view;

			// Without a play session there is nothing to show, and nothing is read.
			CHECK_FALSE(FindGraphLiveTarget(editor, graph, "Counter").has_value());
			view.Update(editor, actions, FindGraphLiveTarget(editor, graph, "Counter"));
			CHECK_FALSE(view.GetTarget().has_value());
			CHECK_FALSE(view.GetValues().has_value());

			editor.SetSelection(std::vector<UUID>{});
			CallOrFail(setup, "play.start", Json{ { "lockstep", true }, { "seed", 3 } });
			CallOrFail(setup, "play.step", Json{ { "ticks", 1 } });
			// The first instance in the play scene's canonical order, until another one holding the graph is selected there.
			const std::optional<GraphLiveTarget> target = FindGraphLiveTarget(editor, graph, "Counter");
			REQUIRE(target.has_value());
			CHECK(target->Entity == first);
			CHECK(target->Script == "Counter");
			REQUIRE(editor.SetSelection({ second }, SceneTarget::Play).has_value());
			CHECK(FindGraphLiveTarget(editor, graph, "Counter")->Entity == second);
			CHECK_FALSE(FindGraphLiveTarget(editor, AssetHandle(0x1234ull), "Other").has_value());

			// A frame submits the read, the safe point between frames runs it, and the next frame takes it.
			view.Update(editor, actions, target);
			CHECK_FALSE(view.GetValues().has_value());
			actions.Pump();
			view.Update(editor, actions, target);
			REQUIRE_MESSAGE(view.GetValues().has_value(), view.GetError());
			CHECK(view.GetError().empty());
			CHECK(view.GetValues()->Tick == 1);
			CHECK(view.GetNodeLines(nodes.Add) == std::vector<std::string>{ "Result = 1" });
			CHECK(view.GetNodeLines(nodes.Count) == std::vector<std::string>{ "Value = 0" });
			CHECK(view.GetNodeLines(nodes.Step).empty()); // an event's flow output holds no value
			CHECK(view.GetNodeValues(nodes.Increment).size() == 1);
			REQUIRE(view.GetValues()->Members.size() == 2);
			CHECK(GraphLiveView::FormatMemberLine(view.GetValues()->Members[0]) == "Count = 1");
			CHECK(GraphLiveView::FormatMemberLine(view.GetValues()->Members[1]) == "Step = 1");

			// The values follow the session from frame to frame.
			CallOrFail(setup, "play.step", Json{ { "ticks", 2 } });
			actions.Pump();
			view.Update(editor, actions, target);
			CHECK(view.GetValues()->Tick == 3);
			CHECK(view.GetNodeLines(nodes.Add) == std::vector<std::string>{ "Result = 3" });

			// Another target forgets the values and reads again; a stopped session ends the polling.
			const GraphLiveTarget other{ .Entity = second, .Script = "Counter" };
			view.Update(editor, actions, other);
			CHECK_FALSE(view.GetValues().has_value());
			actions.Pump();
			view.Update(editor, actions, other);
			REQUIRE(view.GetValues().has_value());
			CHECK(view.GetValues()->Entity.Name == "Second");
			CallOrFail(setup, "play.stop", Json::object());
			actions.Pump();
			view.Update(editor, actions, other);
			CHECK_FALSE(view.GetTarget().has_value());
			CHECK_FALSE(view.GetValues().has_value());

			// A read that fails says why: no graph slot of the entity has the behaviour Door.
			CallOrFail(setup, "play.start", Json{ { "lockstep", true }, { "seed", 3 } });
			const GraphLiveTarget missing{ .Entity = first, .Script = "Door" };
			view.Update(editor, actions, missing);
			actions.Pump();
			view.Update(editor, actions, missing);
			CHECK_FALSE(view.GetValues().has_value());
			CHECK(view.GetError().find("Door") != std::string::npos);
			view.Reset(actions);
			CHECK_FALSE(view.GetTarget().has_value());
			CallOrFail(setup, "play.stop", Json::object());
		}

		TEST_CASE("GraphLiveView: a seeded lockstep play gives the same state hash every tick with the live values shown and hidden")
		{
			Test::AutomationFixture setup("GraphLiveViewHash");
			const CounterNodes nodes = BuildCounter(setup, true);
			CreateCounterEntity(setup, "Counter");
			const AssetHandle graph = FindCounterHandle(setup);
			EditorContext& editor = setup.GetEditor();
			EditorActions actions(editor, setup.GetClient().GetServer());

			// The same seeded lockstep play twice, one tick a frame: once with the canvas showing the live values (its view
			// polled every frame, the read run at each safe point), once without. The Counter moves its entity by its Count, so a
			// read that changed the instance would change the hash of every later tick.
			constexpr uint32_t Ticks = 12;
			const auto play = [&](bool shown)
			{
				GraphLiveView view;
				std::vector<std::string> hashes;
				CallOrFail(setup, "play.start", Json{ { "lockstep", true }, { "seed", 9 } });
				for (uint32_t tick = 1; tick <= Ticks; ++tick)
				{
					const Json stepped = CallOrFail(setup, "play.step", Json{ { "ticks", 1 } });
					hashes.push_back(JsonReader(stepped["stateHash"]).ReadString().value_or(std::string()));
					if (shown)
						view.Update(editor, actions, FindGraphLiveTarget(editor, graph, "Counter"));
					actions.Pump();
					if (shown && tick > 1)
					{
						// The values the last frame's read took, of the tick before.
						REQUIRE_MESSAGE(view.GetValues().has_value(), view.GetError());
						CHECK(view.GetValues()->Tick == tick - 1);
						CHECK(view.GetNodeLines(nodes.Add) == std::vector<std::string>{ std::format("Result = {}", tick - 1) });
						CHECK(view.GetNodeLines(nodes.Place) == std::vector<std::string>{ std::format("Result = ({}, 0, 0)", tick - 1) });
					}
				}
				view.Reset(actions);
				CallOrFail(setup, "play.stop", Json::object());
				return hashes;
			};
			const std::vector<std::string> shown = play(true);
			const std::vector<std::string> hidden = play(false);
			REQUIRE(shown.size() == Ticks);
			CHECK(shown == hidden);
		}

		// W24.2's review: decision 14's "a node whose code raised a retained script error shows the error", and a simulation,
		// which runs no behaviours, shows nothing.
		TEST_CASE("GraphLiveView: a node shows the errors its code raised after its values, and a simulation shows nothing")
		{
			Test::AutomationFixture setup("GraphLiveViewErrors");
			const auto [read, tag] = BuildTagger(setup);
			const Json slot{ { "Script", std::string(TaggerPath) }, { "Fields", Json::object() }, { "ExecutionOrder", 0 } };
			CallOrFail(setup, "entity.create", Json{ { "name", "Tagged" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ slot }) } } } } } });
			const Json info = CallOrFail(setup, "asset.info", Json{ { "asset", std::string(TaggerPath) } });
			const AssetHandle graph = JsonReader(info["asset"]["id"]).ReadUUID().value_or(UUID());
			REQUIRE(graph.IsValid());
			EditorContext& editor = setup.GetEditor();
			EditorActions actions(editor, setup.GetClient().GetServer());
			GraphLiveView view;

			{
				const Test::ExpectLog raised(LogLevel::Error, "attempt to index nil with 'AddTag'");
				CallOrFail(setup, "play.start", Json{ { "lockstep", true }, { "seed", 3 } });
				CallOrFail(setup, "play.step", Json{ { "ticks", 2 } });
			}
			const std::optional<GraphLiveTarget> target = FindGraphLiveTarget(editor, graph, "Tagger");
			REQUIRE(target.has_value());
			view.Update(editor, actions, target);
			actions.Pump();
			view.Update(editor, actions, target);
			REQUIRE_MESSAGE(view.GetValues().has_value(), view.GetError());
			const std::vector<GraphLiveError> errors = view.GetNodeErrors(tag);
			REQUIRE(errors.size() == 1);
			CHECK(errors.front().Node == tag);
			CHECK(errors.front().Count == 1);
			// The AddTag node computes no value, so its one line is its error; the Target node read nil, which the table does
			// not hold, and raised nothing, so it has no line.
			const std::vector<std::string> lines = view.GetNodeLines(tag);
			REQUIRE(lines.size() == 1);
			CHECK(lines.front() == "Error: " + errors.front().Message);
			CHECK(view.GetNodeErrors(read).empty());
			CHECK(view.GetNodeLines(read).empty());
			CallOrFail(setup, "play.stop", Json::object());

			// A simulation runs no behaviours: there is no instance to show, and the view polls nothing.
			CallOrFail(setup, "play.start", Json{ { "lockstep", true }, { "mode", "simulate" } });
			CHECK_FALSE(FindGraphLiveTarget(editor, graph, "Tagger").has_value());
			view.Update(editor, actions, target);
			CHECK_FALSE(view.GetTarget().has_value());
			CHECK_FALSE(view.GetValues().has_value());
			actions.Pump();
			CallOrFail(setup, "play.stop", Json::object());
		}

		// W24.2's integration (stream B's open point): graph.liveValues is among the reads a test run admits
		// (EditorContext::CheckTestRunAdmission), so the canvas shows the test session's values while a test run owns the editor.
		TEST_CASE("GraphLiveView: live values show while a test run owns the editor")
		{
			Test::EditorTestFixture fixture("GraphLiveViewTestRun");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			// The server's clock advances 100 ms a reading, so each pump runs one step of the pending test run.
			auto now = std::chrono::steady_clock::time_point{};
			auto specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient client(fixture.GetEditor(), std::move(specification));
			const CounterNodes nodes = BuildCounter(client);
			CreateCounterEntity(client, "Counter");
			CallOrFail(client, "scene.save", Json::object());
			CallOrFail(client, "script.write", Json{ { "path", "Assets/Tests/Live.test.luau" }, { "source", "return Test.Suite('Live', function() Test.Case('long', function() Test.WaitTicks(1000) end) end)" } });
			CallOrFail(client, "project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Live.test.luau" }, { "Scene", "Assets/Scenes/Main.scene" }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } });
			const AssetHandle graph = FindCounterHandle(client);
			EditorContext& editor = fixture.GetEditor();
			EditorActions actions(editor, client.GetServer());
			GraphLiveView view;

			static_cast<void>(client.Submit("test.run", Json::object()));
			for (int pump = 0; pump < 4; ++pump)
				client.GetServer().Pump();
			REQUIRE(editor.GetPlay().IsTestRunActive());
			const std::optional<GraphLiveTarget> target = FindGraphLiveTarget(editor, graph, "Counter");
			REQUIRE(target.has_value());
			view.Update(editor, actions, target);
			actions.Pump();
			view.Update(editor, actions, target);
			REQUIRE_MESSAGE(view.GetValues().has_value(), view.GetError());
			const uint64_t tick = view.GetValues()->Tick;
			REQUIRE(tick > 0);
			REQUIRE(editor.GetPlay().IsTestRunActive());
			// The test session's instance: each fixed step adds Step (1) to Count.
			CHECK(view.GetNodeLines(nodes.Add) == std::vector<std::string>{ std::format("Result = {}", tick) });
			REQUIRE(view.GetValues()->Members.size() == 2);
			CHECK(GraphLiveView::FormatMemberLine(view.GetValues()->Members[0]) == std::format("Count = {}", tick));

			view.Reset(actions);
			client.GetServer().DisconnectInProcess(client.GetClient());
			CHECK_FALSE(editor.GetPlay().IsTestRunActive());
		}
	}

}
