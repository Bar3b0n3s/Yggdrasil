#include "TestsPCH.h"

#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"

#include "EditorCore/Scripting/ScriptTypeChecker.h"
#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
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

// The graph transpiler (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decisions 7 and 8, stream G-B). The line-map and
// key cases are live (the contract implements them); the others are skipped until stream G-B implements the transpiler and
// commits the fixture graphs with their texts (Tests/Data/Graphs/<Name>.graph and <Name>.luau).

namespace Engine {

	namespace {

		// The fixture graphs whose transpiled bytes are committed: their texts are the output contract's examples, compared byte
		// for byte in the Debug and the Release unit runs (decision 7: one text in every configuration). Bumper reads a component
		// proxy (an optional output) of its event's Entity through Flow.IfNotNil, so the strict check covers optional values.
		constexpr std::string_view FixtureGraphs[] = { "Door", "Counter", "Mover", "Bumper" };

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

		TEST_CASE("GraphTranspiler: the fixture graphs transpile to their committed Luau bytes" * doctest::skip(true))
		{
			const TranspileSetup setup;
			for (const std::string_view name : FixtureGraphs)
			{
				CAPTURE(name);
				const std::string path = std::string("Assets/Scripts/") + std::string(name) + ".graph";
				const Result<GraphTranspileResult> result = TranspileGraph(setup.Read(name), *setup.Library, path);
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

		TEST_CASE("GraphTranspiler: every line maps back to the node whose code it is, and every computed value is a live value" * doctest::skip(true))
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

		TEST_CASE("GraphTranspiler: the text of every fixture graph type-checks without an error" * doctest::skip(true))
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
					CHECK_MESSAGE(diagnostic.Severity != DiagnosticSeverity::Error, diagnostic.Line, ": ", diagnostic.Message);
			}
		}

		TEST_CASE("GraphTranspiler: a graph that does not validate has no text, and the error carries every issue" * doctest::skip(true))
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
