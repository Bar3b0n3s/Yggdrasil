#include "TestsPCH.h"

#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"

#include "EditorCore/Scripting/ScriptTypeChecker.h"
#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// The graph transpiler (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decisions 7 and 8, stream G-B), with the fixture
// graphs and their committed texts (Tests/Data/Graphs/<Name>.graph and <Name>.luau).

namespace Engine {

	namespace {

		// The fixture graphs whose transpiled bytes are committed: their texts are the output contract's examples, compared byte
		// for byte in the Debug and the Release unit runs (decision 7: one text in every configuration). Bumper reads a component
		// proxy (an optional output) of its event's Entity through Flow.IfNotNil, so the strict check covers optional values. M39
		// (Docs/Decisions/0056-m39-graphs-behaviours.md decisions 2 to 6) adds Rune (methods with parameters of several
		// kinds and Any, a result, a Return through a merge, a void early Return, a method without an event node, an
		// OnUiClick Return, an Any variable, Script.Get of a table) and Valve (the world-pause opt-in and the instance
		// chain: Scene.FindByPath, Flow.IfNotNil, Entity:GetScript, Script.Set, Script.Call by name and by index,
		// Script.Get of a call's result and of the instance's member, Self.Instance) and, from the review of W39.1, Paths
		// (every return path the transpiler writes: a returning merge that ends "return false", one returning merge called
		// from another, a Return in a loop's body, typed and nil returns of one method, and a Return entered by several
		// links).
		constexpr std::string_view FixtureGraphs[] = { "Door", "Counter", "Mover", "Bumper", "Rune", "Valve", "Paths" };

		// M24's fixture graphs, which hold none of M39's methods, Returns or opt-in.
		constexpr std::string_view M24FixtureGraphs[] = { "Door", "Counter", "Mover", "Bumper" };

		struct TranspileSetup
		{
			Scope<TypeRegistry> Types = Test::CreateBuiltinRegistry();
			ScriptApiRegistry Api{};
			Scope<GraphNodeLibrary> Library{};

			TranspileSetup()
			{
				const Status registered = RegisterBindings(Api, *Types);
				REQUIRE_MESSAGE(registered.has_value(), registered.error().ToString());
				Result<Scope<GraphNodeLibrary>> library = GraphNodeLibrary::Create(Api);
				REQUIRE_MESSAGE(library.has_value(), library.error().ToString());
				Library = std::move(*library);
			}

			GraphDocument Read(std::string_view name) const
			{
				const Result<std::string> text = Test::ReadTestDataText(std::string("Graphs/") + std::string(name) + ".graph");
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				GraphLoadReport report;
				Result<GraphDocument> graph = GraphFromText(*text, *Types, report, true);
				REQUIRE_MESSAGE(graph.has_value(), graph.error().ToString());
				return std::move(*graph);
			}
		};

		class NoModules final : public IScriptModuleReader
		{
		public:
			Result<std::string> ReadModule(const VfsPath& path) override
			{
				return MakeError(ErrorCode::NotFound, "a graph requires no module ('{}')", path.ToString());
			}
		};

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("GraphTranspiler: a line maps to its node, a layout line and a line outside the text to none, and a live-value key reads back")
		{
			GraphTranspileResult result;
			result.LineNodes = { 0, 0, 4, 4, 7 };
			CHECK(FindGraphNodeAtLine(result, 0) == 0);
			CHECK(FindGraphNodeAtLine(result, 1) == 0);
			CHECK(FindGraphNodeAtLine(result, 3) == 4);
			CHECK(FindGraphNodeAtLine(result, 5) == 7);
			CHECK(FindGraphNodeAtLine(result, 6) == 0);

			CHECK(GetGraphProbeKey(12, "Result") == "12.Result");
			CHECK(ParseGraphProbeKey("12.Result") == GraphPinRef{ .Node = 12, .Pin = "Result" });
			CHECK(ParseGraphProbeKey("7.Result1") == GraphPinRef{ .Node = 7, .Pin = "Result1" });
			for (const std::string_view bad : { "", "12", "12.", ".Result", "0.Result", "012.Result", "-1.Result", "x.Result", "4294967295.Result" })
				CHECK_MESSAGE(!ParseGraphProbeKey(bad).has_value(), bad);
		}

		TEST_CASE("GraphTranspiler: the fixture graphs transpile to their committed Luau bytes")
		{
			const TranspileSetup setup;
			for (const std::string_view name : FixtureGraphs)
			{
				CAPTURE(name);
				const std::string path = std::string("Assets/Scripts/") + std::string(name) + ".graph";
				const GraphDocument graph = setup.Read(name);
				// The fixture files are canonical: the writer gives their bytes back.
				const Result<std::string> source = Test::ReadTestDataText(std::string("Graphs/") + std::string(name) + ".graph");
				REQUIRE(source.has_value());
				CHECK(GraphToText(graph, *setup.Types).value_or(std::string()) == *source);
				const Result<GraphTranspileResult> result = TranspileGraph(graph, *setup.Library, path);
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				const Result<std::string> expected = Test::ReadTestDataText(std::string("Graphs/") + std::string(name) + ".luau");
				REQUIRE_MESSAGE(expected.has_value(), expected.error().ToString());
				CHECK(result->Luau == *expected);
				// The layout's fixed lines (decision 7).
				CHECK(result->Luau.starts_with("--!strict\n-- Generated from " + path + " by the graph transpiler: edit the graph, not this text.\n"));
				CHECK(result->Luau.ends_with("\nreturn Script.Define(\"" + std::string(name) + "\", Graph)\n"));
				// Transpiling again gives the same result.
				CHECK(TranspileGraph(setup.Read(name), *setup.Library, path).value_or(GraphTranspileResult()) == *result);
			}
		}

		// M39 (decision 9): GraphImporter's version stays 1 because a graph without methods, Returns or the world-pause opt-in
		// transpiles to the text it had, so nothing imports again and no .meta changes: M24's fixture texts are M24's bytes, and
		// a version 1 file (Counter at version 1) gives Counter's text.
		TEST_CASE("GraphTranspiler: a graph without methods or the opt-in keeps its text byte for byte")
		{
			const TranspileSetup setup;
			for (const std::string_view name : M24FixtureGraphs)
			{
				CAPTURE(name);
				const Result<GraphTranspileResult> result = TranspileGraph(setup.Read(name), *setup.Library, std::string("Assets/Scripts/") + std::string(name) + ".graph");
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				const Result<std::string> expected = Test::ReadTestDataText(std::string("Graphs/") + std::string(name) + ".luau");
				REQUIRE(expected.has_value());
				CHECK(result->Luau == *expected);
				for (const std::string_view added : { "RunsWhileWorldPaused", "return nil", "): boolean", "result", "p_" })
					CHECK_MESSAGE(result->Luau.find(added) == std::string::npos, added);
			}
			const Result<std::string> v1 = Test::ReadTestDataText("Formats/Graph/v1.graph");
			REQUIRE(v1.has_value());
			GraphLoadReport report;
			const Result<GraphDocument> counter = GraphFromText(*v1, *setup.Types, report, true);
			REQUIRE_MESSAGE(counter.has_value(), counter.error().ToString());
			CHECK(report.FileVersion == 1);
			const Result<GraphTranspileResult> text = TranspileGraph(*counter, *setup.Library, "Assets/Scripts/Counter.graph");
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			CHECK(text->Luau == Test::ReadTestDataText("Graphs/Counter.luau").value_or(std::string()));
		}

		TEST_CASE("GraphTranspiler: every line maps back to the node whose code it is, and every computed value is a live value")
		{
			const TranspileSetup setup;
			const GraphDocument door = setup.Read("Door");
			const Result<GraphTranspileResult> result = TranspileGraph(door, *setup.Library, "Assets/Scripts/Door.graph");
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(result->LineNodes.size() == static_cast<size_t>(std::ranges::count(result->Luau, '\n')));
			// Each node's code starts with its comment line, which the map gives to it.
			for (const GraphNode& node : door.Nodes)
			{
				const std::string comment = "-- " + std::to_string(node.Id) + ": " + node.Kind + "\n";
				const size_t at = result->Luau.find(comment);
				if (at == std::string::npos)
					continue; // a node with no code of its own (an event's flow starts in its callback's function)
				const uint32_t line = static_cast<uint32_t>(std::count(result->Luau.begin(), result->Luau.begin() + static_cast<std::ptrdiff_t>(at), '\n')) + 1;
				CHECK(FindGraphNodeAtLine(*result, line) == node.Id);
			}
			// Every probe is a computed value, written under its key.
			std::set<std::string> keys;
			for (const GraphProbe& probe : result->Probes)
			{
				CHECK(probe.Key == GetGraphProbeKey(probe.Node, probe.Pin));
				CHECK(result->Luau.find("\"" + probe.Key + "\"") != std::string::npos);
				keys.insert(probe.Key);
			}
			CHECK(keys.contains("2.Result")); // Compare.Equal
			CHECK(keys.contains("3.Value"));  // Self.Get.Player
			CHECK(std::ranges::is_sorted(result->Probes, [](const GraphProbe& left, const GraphProbe& right)
			{
				return std::tie(left.Node, left.Pin) < std::tie(right.Node, right.Pin);
			}));
		}

		TEST_CASE("GraphTranspiler: the text of every fixture graph type-checks without an error")
		{
			const TranspileSetup setup;
			const Result<Scope<ScriptTypeChecker>> checker = ScriptTypeChecker::Create(setup.Api, {});
			REQUIRE_MESSAGE(checker.has_value(), checker.error().ToString());
			for (const std::string_view name : FixtureGraphs)
			{
				CAPTURE(name);
				const std::string path = std::string("Assets/Scripts/") + std::string(name) + ".graph";
				const Result<GraphTranspileResult> result = TranspileGraph(setup.Read(name), *setup.Library, path);
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				NoModules modules;
				const Result<VfsPath> vfsPath = VfsPath::Create("project", path);
				REQUIRE(vfsPath.has_value());
				const std::vector<ScriptDiagnostic> diagnostics = (*checker)->CheckScript({ .Path = *vfsPath, .Source = result->Luau, .Modules = &modules });
				for (const ScriptDiagnostic& diagnostic : diagnostics)
				{
					CHECK_MESSAGE(diagnostic.Severity != DiagnosticSeverity::Error, diagnostic.Line, ": ", diagnostic.Message);
					// No warning either, Luau's lints included: the returns M39 writes add no unreachable line of their own
					// (Docs/Decisions/0056-m39-graphs-behaviours.md decision 4).
					CHECK_MESSAGE(diagnostic.Severity != DiagnosticSeverity::Warning, diagnostic.Line, ": ", diagnostic.Message);
				}
			}
		}

		// Docs/Decisions/0035-m24-contract.md risk 1: the transpiler builds a graph at MaxGraphNodes' scale without recursing per
		// node; a flow nested deeper than Luau's parser recurses on an import worker's stack is refused at its node; and a text
		// that exceeds another of Luau's limits (a function's 200 locals) fails its compile at a line the line map gives to a
		// node, which the import reports (decision 8), never at run time.
		TEST_CASE("GraphTranspiler: a graph at MaxGraphNodes' scale transpiles, a flow nested too deep is refused at its node, and a text beyond Luau's limits fails its compile at a node's line")
		{
			const TranspileSetup setup;
			const auto compile = [](const GraphTranspileResult& text)
			{
				const Result<VfsPath> path = VfsPath::Create("project", "Assets/Scripts/Large.graph");
				REQUIRE(path.has_value());
				return ScriptCompiler::Compile({ .Path = *path, .Source = text.Luau });
			};

			// A flow of MaxGraphNodes - 1 statements, each after the last.
			GraphDocument chain;
			chain.Name = "Chain";
			chain.Nodes.push_back(GraphNode{ .Id = 1, .Kind = "Event.OnStart" });
			for (uint32_t id = 2; id <= MaxGraphNodes; ++id)
			{
				chain.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Call.Log.Info", .Values = { { "Values", VariantValue(Json(id)) } } });
				chain.Links.push_back(GraphLink{ .From = { id - 1, "Then" }, .To = { id, "In" } });
			}
			const Result<GraphTranspileResult> flat = TranspileGraph(chain, *setup.Library, "Assets/Scripts/Large.graph");
			REQUIRE_MESSAGE(flat.has_value(), flat.error().ToString());
			CHECK(flat->LineNodes.size() == static_cast<size_t>(std::ranges::count(flat->Luau, '\n')));
			const Result<ScriptCompilation> compiled = compile(*flat);
			CHECK_MESSAGE(compiled.has_value(), (compiled ? "" : compiled.error().ToString()));

			// Branches nested deeper than the script compiler parses on an import worker's stack: refused at the node that goes
			// too deep, which ValidateGraph reports (W24.1's integration), so the graph transpiles exactly when it validates.
			GraphDocument nested;
			nested.Name = "Nested";
			nested.Nodes.push_back(GraphNode{ .Id = 1, .Kind = "Event.OnStart" });
			constexpr uint32_t Depth = 200;
			for (uint32_t id = 2; id <= Depth; ++id)
			{
				nested.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Flow.Branch" });
				nested.Links.push_back(GraphLink{ .From = { id - 1, id == 2 ? "Then" : "True" }, .To = { id, "In" } });
			}
			const Result<GraphTranspileResult> deep = TranspileGraph(nested, *setup.Library, "Assets/Scripts/Large.graph");
			REQUIRE_FALSE(deep.has_value());
			CHECK(deep.error().GetCode() == ErrorCode::Validation);
			REQUIRE(deep.error().GetIssues().size() == 1);
			const std::vector<GraphIssue> issues = ValidateGraph(nested, *setup.Library);
			REQUIRE(issues.size() == 1);
			CHECK_FALSE(issues.front().Incomplete);
			CHECK(deep.error().GetIssues().front().JsonPointer == issues.front().JsonPointer);
			CHECK(issues.front().JsonPointer.starts_with("/Nodes/"));

			// One statement reading a chain of pure values longer than a function's locals allow (from a field, so that Luau's
			// constant folding keeps every value a local).
			GraphDocument values;
			values.Name = "Values";
			values.Fields = { GraphField{ .Name = "Base", .Kind = GraphValueKind::Number, .Default = VariantValue(Json(0)) } };
			values.Nodes.push_back(GraphNode{ .Id = 1, .Kind = "Event.OnStart" });
			values.Nodes.push_back(GraphNode{ .Id = 2, .Kind = "Call.Log.Info" });
			values.Links.push_back(GraphLink{ .From = { 1, "Then" }, .To = { 2, "In" } });
			for (uint32_t id = 3; id < MaxGraphNodes; ++id)
			{
				values.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Number.Add", .Values = { { "B", VariantValue(Json(1)) } } });
				values.Links.push_back(GraphLink{ .From = { id, "Result" }, .To = { id == 3 ? 2 : id - 1, id == 3 ? "Values" : "A" } });
			}
			values.Nodes.push_back(GraphNode{ .Id = static_cast<uint32_t>(MaxGraphNodes), .Kind = "Self.Get.Base" });
			values.Links.push_back(GraphLink{ .From = { static_cast<uint32_t>(MaxGraphNodes), "Value" }, .To = { static_cast<uint32_t>(MaxGraphNodes) - 1, "A" } });
			const Result<GraphTranspileResult> wide = TranspileGraph(values, *setup.Library, "Assets/Scripts/Large.graph");
			REQUIRE_MESSAGE(wide.has_value(), wide.error().ToString());
			CHECK(wide->Probes.size() == MaxGraphNodes - 2);
			const Result<ScriptCompilation> refusedLocals = compile(*wide);
			REQUIRE_FALSE(refusedLocals.has_value());
			const uint32_t node = FindGraphNodeAtLine(*wide, refusedLocals.error().GetLocation().Line);
			CHECK(node >= 3);
		}

		TEST_CASE("GraphTranspiler: a graph that does not validate has no text, and the error carries every issue")
		{
			const TranspileSetup setup;
			GraphDocument door = setup.Read("Door");
			door.Links.erase(door.Links.begin() + 1); // the comparison's A is no longer linked: incomplete
			door.Nodes[3].Kind = "Flow.Nope";         // an unknown kind
			const Result<GraphTranspileResult> result = TranspileGraph(door, *setup.Library, "Assets/Scripts/Door.graph");
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Validation);
			CHECK(result.error().GetIssues().size() == ValidateGraph(door, *setup.Library).size());
			CHECK(result.error().GetIssues().size() >= 2);
		}
	}

}
