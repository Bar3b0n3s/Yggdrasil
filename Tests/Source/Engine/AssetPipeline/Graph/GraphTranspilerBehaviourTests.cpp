#include "TestsPCH.h"

#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiInteraction.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"
#include "Support/SceneTestFixture.h"
#include "Support/ScriptTestFixture.h"
#include "Support/TestData.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// M39's graphs as behaviours that run (Docs/Decisions/0056-m39-graphs-behaviours.md decisions 2 to 6; World Tree's brief 18):
// graphs transpiled and compiled under their .graph paths beside Luau behaviours, so the members, methods, returns and the
// world-pause opt-in are checked as they run, in the script engine and a play session.

namespace Engine {

	namespace {

		// A node library of the engine's script API, and the transpiler over it.
		struct BehaviourGraphs
		{
			Scope<TypeRegistry> Types = Test::CreateBuiltinRegistry();
			ScriptApiRegistry Api{};
			Scope<GraphNodeLibrary> Library{};

			BehaviourGraphs()
			{
				const Status registered = RegisterBindings(Api, *Types);
				REQUIRE_MESSAGE(registered.has_value(), registered.error().ToString());
				Result<Scope<GraphNodeLibrary>> library = GraphNodeLibrary::Create(Api);
				REQUIRE_MESSAGE(library.has_value(), library.error().ToString());
				Library = std::move(*library);
			}

			GraphTranspileResult Transpile(const GraphDocument& graph, std::string_view path) const
			{
				Result<GraphTranspileResult> text = TranspileGraph(graph, *Library, path);
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				return std::move(*text);
			}

			GraphDocument ReadFixture(std::string_view name) const
			{
				const Result<std::string> text = Test::ReadTestDataText(std::format("Graphs/{}.graph", name));
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				GraphLoadReport report;
				Result<GraphDocument> graph = GraphFromText(*text, *Types, report, true);
				REQUIRE_MESSAGE(graph.has_value(), graph.error().ToString());
				return std::move(*graph);
			}

			// Transpiles `graph` and compiles its text under `path` into the fixture's script `handle`.
			GraphTranspileResult Add(Test::ScriptTestFixture& fixture, AssetHandle handle, const GraphDocument& graph, std::string path) const
			{
				GraphTranspileResult text = Transpile(graph, path);
				const Result<AssetRef<ScriptData>> added = fixture.AddScript(handle, std::move(path), text.Luau);
				REQUIRE_MESSAGE(added.has_value(), added.error().ToString());
				return text;
			}
		};

		GraphNode MakeNode(uint32_t id, std::string kind, std::map<std::string, VariantValue> values = {})
		{
			return GraphNode{ .Id = id, .Kind = std::move(kind), .Position = glm::vec2(0.0f), .Values = std::move(values) };
		}

		GraphLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
		{
			return GraphLink{ .From = { .Node = from, .Pin = std::move(fromPin) }, .To = { .Node = to, .Pin = std::move(toPin) } };
		}

		VariantValue Literal(Json value)
		{
			return VariantValue(std::move(value));
		}

		GraphVariable MakeVariable(std::string name, GraphValueKind kind, Json value)
		{
			return GraphVariable{ .Name = std::move(name), .Kind = kind, .Default = VariantValue(std::move(value)), .Element = GraphValueKind::Number, .AssetType = {}, .EnumValues = {} };
		}

		// A canonical graph of these parts.
		GraphDocument MakeGraph(std::string name, std::vector<GraphNode> nodes, std::vector<GraphLink> links, std::vector<GraphVariable> variables = {},
			std::vector<GraphMethod> methods = {})
		{
			GraphDocument graph;
			graph.Name = std::move(name);
			graph.Variables = std::move(variables);
			graph.Methods = std::move(methods);
			graph.Nodes = std::move(nodes);
			graph.Links = std::move(links);
			CanonicalizeGraph(graph);
			REQUIRE_MESSAGE(ValidateGraphStructure(graph).has_value(), ValidateGraphStructure(graph).error().ToString());
			return graph;
		}

		void AttachScripts(Test::ScriptTestFixture& fixture, Entity entity, std::string_view slots)
		{
			const Result<Json> json = JsonReader::Parse(std::string(R"({ "Scripts": )") + std::string(slots) + "}");
			REQUIRE(json.has_value());
			const Status added = ComponentAccess::AddComponent(entity, "Script", &*json, fixture.GetSchemaSnapshot().get());
			REQUIRE_MESSAGE(added.has_value(), (added ? std::string() : added.error().ToString()));
		}

		std::string Slot(AssetHandle script)
		{
			return std::format(R"([ {{ "Script": "{}" }} ])", script.ToString());
		}

		// The string an evaluation in the fixture's running engine returns.
		std::string EvaluateText(Test::ScriptTestFixture& fixture, std::string_view source)
		{
			const Result<ScriptEvaluation> evaluated = fixture.Evaluate(source);
			REQUIRE_MESSAGE(evaluated.has_value(), evaluated.error().ToString());
			const Result<std::string> text = JsonReader(evaluated->Value.Get()).ReadString();
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			return *text;
		}

		// The number an evaluation returns in a play session.
		double EvaluateNumber(PlaySession& session, std::string_view source)
		{
			const Result<ScriptEvaluation> evaluated = session.GetScripts()->Evaluate(source, {}, std::nullopt);
			REQUIRE_MESSAGE(evaluated.has_value(), evaluated.error().ToString());
			const Result<double> value = JsonReader(evaluated->Value.Get()).ReadDouble();
			REQUIRE_MESSAGE(value.has_value(), value.error().ToString());
			return *value;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		// Decision 2 and brief 18's SteamValve and GameFlow checks: the Valve fixture graph finds the press by path
		// (Scene.FindByPath, Flow.IfNotNil, Entity:GetScript), writes its Running, calls its Toggle with the graph's own
		// instance and reads a member of the table it returns, and calls a method whose name is no Luau name by index.
		TEST_CASE("GraphTranspiler: Script.Get, Script.Set and Script.Call read, write and call a behaviour's members")
		{
			const BehaviourGraphs graphs;
			Test::ScriptTestFixture fixture;
			const AssetHandle press(0x390001);
			const AssetHandle valve(0x390002);
			REQUIRE(fixture.AddScript(press, "Assets/Scripts/Press.luau", R"(local P = {}
P.Fields = { Running = Field.Bool(true), Speed = Field.Number(2) }
function P:Toggle(by)
	self.By = by
	return { Pressure = self.Speed * 10 }
end
P["Speed-Up"] = function(self, first, second)
	self.SpeedUpFirst = first
	self.SpeedUpSecond = second
end
return Script.Define("Press", P)
)"));
			static_cast<void>(graphs.Add(fixture, valve, graphs.ReadFixture("Valve"), "Assets/Scripts/Valve.graph"));
			Scene& scene = fixture.GetScene();
			AttachScripts(fixture, scene.CreateEntity("Press"), Slot(press));
			AttachScripts(fixture, scene.CreateEntity("Valve"), Slot(valve));
			REQUIRE(fixture.Start());
			CHECK(fixture.Errors.empty());

			CHECK(EvaluateText(fixture, R"(local press = Scene.FindByPath("/Press"):GetScript("Press")
local valve = Scene.FindByPath("/Valve"):GetScript("Valve")
return table.concat({ tostring(press.Running), tostring(press.By == valve), tostring(valve.Pressure), tostring(press.SpeedUpFirst),
	tostring(press.SpeedUpSecond) }, ","))")
				== "false,true,20,nil,2");
			CHECK(fixture.Errors.empty());
		}

		// Decision 2: a member a table lacks reads nil, which Flow.IfNotNil tells apart; calling a method the instance lacks and
		// reading a member an engine value lacks are Luau's run-time errors, each at a line the text's map gives to its node.
		TEST_CASE("GraphTranspiler: a member a table lacks reads nil, and a missing method is a run-time error located at its node")
		{
			const BehaviourGraphs graphs;
			Test::ScriptTestFixture fixture;
			const AssetHandle probeScript(0x390003);
			const AssetHandle strictScript(0x390004);
			// Probe: OnStart reads its own instance's member Missing (nil: SawNil is set on the False flow), then calls its
			// method Nope (node 8).
			const GraphDocument probe = MakeGraph("Probe",
				{ MakeNode(1, "Event.OnStart"), MakeNode(2, "Flow.Sequence"), MakeNode(3, "Flow.IfNotNil"), MakeNode(4, "Script.Get", { { "Member", Literal("Missing") } }),
					MakeNode(5, "Self.Instance"), MakeNode(6, "Self.Set.SawNil", { { "Value", Literal(true) } }), MakeNode(8, "Script.Call", { { "Method", Literal("Nope") } }) },
				{ MakeLink(1, "Then", 2, "In"), MakeLink(2, "Then1", 3, "In"), MakeLink(5, "Value", 4, "Object"), MakeLink(4, "Value", 3, "Value"),
					MakeLink(3, "False", 6, "In"), MakeLink(2, "Then2", 8, "In"), MakeLink(5, "Value", 8, "Object") },
				{ MakeVariable("SawNil", GraphValueKind::Bool, Json(false)) });
			// Strict: OnStart reads the member Bogus of its Entity (node 3), which the engine value lacks.
			const GraphDocument strict = MakeGraph("Strict",
				{ MakeNode(1, "Event.OnStart"), MakeNode(2, "Self.Set.Read"), MakeNode(3, "Script.Get", { { "Member", Literal("Bogus") } }), MakeNode(4, "Self.Entity") },
				{ MakeLink(1, "Then", 2, "In"), MakeLink(3, "Value", 2, "Value"), MakeLink(4, "Value", 3, "Object") },
				{ MakeVariable("Read", GraphValueKind::Any, Json(nullptr)) });
			const GraphTranspileResult probeText = graphs.Add(fixture, probeScript, probe, "Assets/Scripts/Probe.graph");
			const GraphTranspileResult strictText = graphs.Add(fixture, strictScript, strict, "Assets/Scripts/Strict.graph");
			Scene& scene = fixture.GetScene();
			AttachScripts(fixture, scene.CreateEntity("Probe"), Slot(probeScript));
			AttachScripts(fixture, scene.CreateEntity("Strict"), Slot(strictScript));
			const Test::ExpectLog missingMethod(LogLevel::Error, "attempt to call missing method 'Nope'");
			const Test::ExpectLog missingMember(LogLevel::Error, "unknown Entity member 'Bogus'");
			REQUIRE(fixture.Start());

			CHECK(EvaluateText(fixture, R"(return tostring(Scene.FindByPath("/Probe"):GetScript("Probe").SawNil))") == "true");
			REQUIRE(fixture.Errors.size() == 2);
			for (const ScriptError& error : fixture.Errors)
			{
				CAPTURE(error.Message);
				if (error.Script == "Assets/Scripts/Probe.graph")
					CHECK(FindGraphNodeAtLine(probeText, error.Line) == 8);
				else
				{
					CHECK(error.Script == "Assets/Scripts/Strict.graph");
					CHECK(FindGraphNodeAtLine(strictText, error.Line) == 3);
					CHECK(error.Message.find("Bogus") != std::string::npos);
				}
			}
		}

		// Decision 3 and brief 18's KindlingBrazier and rune stone checks: a Luau Health calls the Rune fixture's methods on its
		// entity's graph: Blocks(hit) returns true for an ice or stone hit (through its merge) and nil otherwise, Damaged(hit,
		// amount) counts a blow and keeps the hit's Kind (a table's member) unless the amount is below 1 (a void early
		// Return), and Count, a method without an event node, returns nil.
		TEST_CASE("GraphTranspiler: a graph method returns its result to a Luau caller")
		{
			const BehaviourGraphs graphs;
			Test::ScriptTestFixture fixture;
			const AssetHandle health(0x390005);
			const AssetHandle rune(0x390006);
			REQUIRE(fixture.AddScript(health, "Assets/Scripts/Health.luau", R"(local H = {}
function H:Damage(hit)
	local rune = self.Entity:GetScript("Rune")
	local blocked = rune:Blocks(hit)
	if not blocked then
		rune:Damaged(hit, hit.Amount)
	end
	return blocked
end
return Script.Define("Health", H)
)"));
			static_cast<void>(graphs.Add(fixture, rune, graphs.ReadFixture("Rune"), "Assets/Scripts/Rune.graph"));
			AttachScripts(fixture, fixture.GetScene().CreateEntity("Crystal"),
				std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", health.ToString(), rune.ToString()));
			REQUIRE(fixture.Start());

			CHECK(EvaluateText(fixture, R"(local crystal = Scene.FindByPath("/Crystal")
local health = crystal:GetScript("Health")
local rune = crystal:GetScript("Rune")
local results = {
	tostring(health:Damage({ Kind = "Ice", Amount = 3 })),
	tostring(health:Damage({ Kind = "Fire", Amount = 3 })),
	tostring(health:Damage({ Kind = "Stone", Amount = 3 })),
	tostring(health:Damage({ Kind = "Bolt", Amount = 0.5 })),
	tostring(rune.Blows), tostring(rune.LastKind), tostring(rune:Count()),
}
return table.concat(results, ","))")
				== "true,nil,true,nil,1,Fire,nil");
			CHECK(fixture.Errors.empty());
		}

		// Decision 4: a merge whose flow reaches a Return returns its result through the flag its calls test, and the statements
		// after the merge's call (the Sequence's second flow) do not run.
		TEST_CASE("GraphTranspiler: a Return through a merge ends its method")
		{
			const BehaviourGraphs graphs;
			Test::ScriptTestFixture fixture;
			const AssetHandle gate(0x390007);
			// Check(flag): a Sequence whose first flow branches on flag into one merge (node 5, entered by both branches) that
			// counts and returns 10, and whose second flow would set After.
			const GraphDocument graph = MakeGraph("Gate",
				{ MakeNode(1, "Method.Check"), MakeNode(2, "Flow.Sequence"), MakeNode(3, "Flow.Branch"), MakeNode(5, "Self.Set.Count"), MakeNode(6, "Number.Add", { { "B", Literal(1) } }),
					MakeNode(7, "Self.Get.Count"), MakeNode(8, "Return.Check", { { "Value", Literal(10) } }), MakeNode(9, "Self.Set.After", { { "Value", Literal(true) } }) },
				{ MakeLink(1, "Then", 2, "In"), MakeLink(1, "flag", 3, "Condition"), MakeLink(2, "Then1", 3, "In"), MakeLink(3, "True", 5, "In"), MakeLink(3, "False", 5, "In"),
					MakeLink(7, "Value", 6, "A"), MakeLink(6, "Result", 5, "Value"), MakeLink(5, "Then", 8, "In"), MakeLink(2, "Then2", 9, "In") },
				{ MakeVariable("After", GraphValueKind::Bool, Json(false)), MakeVariable("Count", GraphValueKind::Number, Json(0)) },
				{ GraphMethod{ .Name = "Check",
					.Parameters = { GraphParameter{ .Name = "flag", .Kind = GraphValueKind::Bool, .AssetType = {} } },
					.Results = { GraphResult{ .Kind = GraphValueKind::Number, .AssetType = {} } } } });
			const GraphTranspileResult text = graphs.Add(fixture, gate, graph, "Assets/Scripts/Gate.graph");
			CHECK(text.Luau.find("local function flow5(): boolean") != std::string::npos);
			CHECK(text.Luau.find("if flow5() then return result end") != std::string::npos);
			AttachScripts(fixture, fixture.GetScene().CreateEntity("Gate"), Slot(gate));
			REQUIRE(fixture.Start());

			CHECK(EvaluateText(fixture, R"(local gate = Scene.FindByPath("/Gate"):GetScript("Gate")
local first = gate:Check(true)
local second = gate:Check(false)
return table.concat({ tostring(first), tostring(second), tostring(gate.Count), tostring(gate.After) }, ","))")
				== "10,10,2,false");
			CHECK(fixture.Errors.empty());
		}

		// Decision 4 (closing Docs/Backlog.md §6's "a graph's UI event never consumes the event"): a graph's Return.OnUiClick
		// of true stops the click after its entity, and of false (or nil) lets it bubble to the parent's behaviour.
		TEST_CASE("GraphTranspiler: an OnUiClick Return of true consumes the click")
		{
			const BehaviourGraphs graphs;
			Test::ScriptTestFixture fixture;
			const AssetHandle counter(0x390008);
			const AssetHandle button(0x390009);
			REQUIRE(fixture.AddScript(counter, "Assets/Scripts/Bubbles.luau", R"(local B = {}
function B:OnCreate()
	self.Heard = 0
end
function B:OnUiClick(element)
	self.Heard += 1
end
return Script.Define("Bubbles", B)
)"));
			// Button: counts its clicks and returns its Consume field.
			GraphDocument graph = MakeGraph("Button",
				{ MakeNode(1, "Event.OnUiClick"), MakeNode(2, "Self.Set.Clicks"), MakeNode(3, "Number.Add", { { "B", Literal(1) } }), MakeNode(4, "Self.Get.Clicks"),
					MakeNode(5, "Return.OnUiClick"), MakeNode(6, "Self.Get.Consume") },
				{ MakeLink(1, "Then", 2, "In"), MakeLink(4, "Value", 3, "A"), MakeLink(3, "Result", 2, "Value"), MakeLink(2, "Then", 5, "In"), MakeLink(6, "Value", 5, "Value") },
				{ MakeVariable("Clicks", GraphValueKind::Number, Json(0)) });
			graph.Fields = { GraphField{ .Name = "Consume", .Kind = GraphValueKind::Bool, .Default = VariantValue(Json(true)), .Element = GraphValueKind::Number, .AssetType = {}, .EnumValues = {}, .Options = {} } };
			static_cast<void>(graphs.Add(fixture, button, graph, "Assets/Scripts/Button.graph"));
			Scene& scene = fixture.GetScene();
			const Entity menu = scene.CreateEntityWithID(UUID(10), "Menu");
			const Entity consuming = scene.CreateEntityWithID(UUID(20), "Consuming", menu);
			const Entity bubbling = scene.CreateEntityWithID(UUID(30), "Bubbling", menu);
			AttachScripts(fixture, menu, Slot(counter));
			AttachScripts(fixture, consuming, Slot(button));
			AttachScripts(fixture, bubbling, std::format(R"([ {{ "Script": "{}", "Fields": {{ "Consume": false }} }} ])", button.ToString()));
			REQUIRE(fixture.Start());

			fixture.GetEngine()->DispatchUiEvents(std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Click, .Target = consuming.GetUUID() } });
			CHECK(EvaluateText(fixture, R"(return tostring(Scene.FindByPath("/Menu"):GetScript("Bubbles").Heard) .. "," .. tostring(Scene.FindByPath("/Menu/Consuming"):GetScript("Button").Clicks))")
				== "0,1");
			fixture.GetEngine()->DispatchUiEvents(std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Click, .Target = bubbling.GetUUID() } });
			CHECK(EvaluateText(fixture, R"(return tostring(Scene.FindByPath("/Menu"):GetScript("Bubbles").Heard) .. "," .. tostring(Scene.FindByPath("/Menu/Bubbling"):GetScript("Button").Clicks))")
				== "1,1");
			CHECK(fixture.Errors.empty());
		}

		// Decision 6 (closing Docs/Backlog.md §6's graph row on the world pause): a graph that opts in keeps counting its fixed
		// updates while a Luau menu holds the world paused, and one that does not holds.
		TEST_CASE("GraphTranspiler: a graph that opts in runs while the world is paused")
		{
			const BehaviourGraphs graphs;
			Test::ScriptTestFixture fixture;
			const AssetHandle menu(0x39000a);
			const AssetHandle ticker(0x39000b);
			const AssetHandle holder(0x39000c);
			REQUIRE(fixture.AddScript(menu, "Assets/Scripts/Menu.luau", R"(local M = { RunsWhileWorldPaused = true }
function M:OnFixedUpdate()
	if Time.GetSessionTick() == 5 then Time.PauseWorld() end
end
return Script.Define("Menu", M)
)"));
			const auto counting = [](std::string name, bool opts)
			{
				GraphDocument graph = MakeGraph(std::move(name),
					{ MakeNode(1, "Event.OnFixedUpdate"), MakeNode(2, "Self.Set.Ticks"), MakeNode(3, "Number.Add", { { "B", Literal(1) } }), MakeNode(4, "Self.Get.Ticks") },
					{ MakeLink(1, "Then", 2, "In"), MakeLink(4, "Value", 3, "A"), MakeLink(3, "Result", 2, "Value") }, { MakeVariable("Ticks", GraphValueKind::Number, Json(0)) });
				graph.RunsWhileWorldPaused = opts;
				return graph;
			};
			const GraphTranspileResult tickerText = graphs.Add(fixture, ticker, counting("Ticker", true), "Assets/Scripts/Ticker.graph");
			const GraphTranspileResult holderText = graphs.Add(fixture, holder, counting("Holder", false), "Assets/Scripts/Holder.graph");
			CHECK(tickerText.Luau.find("\nGraph.RunsWhileWorldPaused = true\n") != std::string::npos);
			CHECK(holderText.Luau.find("RunsWhileWorldPaused") == std::string::npos);
			Scene& scene = fixture.GetScene();
			AttachScripts(fixture, scene.CreateEntity("Menu"), Slot(menu));
			AttachScripts(fixture, scene.CreateEntity("Ticker"), Slot(ticker));
			AttachScripts(fixture, scene.CreateEntity("Holder"), Slot(holder));

			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 39;
			specification.Serial = 39;
			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			PlaySession& session = **started;
			session.SetExtractionEnabled(false);
			const auto run = [&session](uint32_t ticks)
			{
				for (uint32_t tick = 0; tick < ticks; ++tick)
				{
					session.FixedStep();
					const double delta = session.GetFixedDelta();
					session.FrameUpdate(FrameTime{ .DeltaTime = delta, .UnscaledDeltaTime = delta, .Alpha = 1.0, .FrameIndex = 0 });
				}
			};
			run(10);
			REQUIRE(session.IsWorldPaused());
			const double ticker10 = EvaluateNumber(session, "return Scene.FindByPath(\"/Ticker\"):GetScript(\"Ticker\").Ticks");
			const double holder10 = EvaluateNumber(session, "return Scene.FindByPath(\"/Holder\"):GetScript(\"Holder\").Ticks");
			run(10);
			CHECK(EvaluateNumber(session, "return Scene.FindByPath(\"/Ticker\"):GetScript(\"Ticker\").Ticks") == ticker10 + 10.0);
			CHECK(EvaluateNumber(session, "return Scene.FindByPath(\"/Holder\"):GetScript(\"Holder\").Ticks") == holder10);
			CHECK(holder10 < ticker10);
			CHECK(session.GetScriptErrors().GetErrors().empty());
		}
	}

}
