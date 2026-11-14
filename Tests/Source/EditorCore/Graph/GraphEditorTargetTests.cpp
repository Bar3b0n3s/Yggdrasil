#include "TestsPCH.h"

#include "EditorCore/Graph/GraphEditorTarget.h"

#include "EditorCore/Automation/GraphMethods.h"
#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

// What the graph editor opens (M24, W24.2's stream A; Docs/Decisions/0035-m24-contract.md decision 15, C24.1's review finding 16):
// the node a target names, as graph.read's diagnostics and line map give it, and which assets are graphs.

namespace Engine {

	namespace {

		// A graph.read result of three nodes (ids 4, 9 and 12) with a line map and three findings.
		GraphReadResult MakeRead()
		{
			GraphReadResult read;
			for (const uint32_t id : { 4u, 9u, 12u })
				read.Document.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Flow.Branch", .Position = glm::vec2(0.0f), .Values = {} });
			read.LineNodes = { 0, 0, 9, 9, 12, 77, 0 };
			read.Diagnostics = {
				GraphDiagnostic{ .Severity = DiagnosticSeverity::Error, .Code = std::string(GraphInvalidCode), .Message = "the input is not linked", .Node = 12, .JsonPointer = "/Nodes/2/Values/Condition", .Line = 0, .Incomplete = true },
				GraphDiagnostic{ .Severity = DiagnosticSeverity::Error, .Code = std::string(GraphInvalidCode), .Message = "the graph has no name", .Node = 0, .JsonPointer = {}, .Line = 0, .Incomplete = false },
				GraphDiagnostic{ .Severity = DiagnosticSeverity::Warning, .Code = std::string(GraphUnreachableNodeCode), .Message = "node 4 never runs", .Node = 4, .JsonPointer = "/Nodes/0", .Line = 0, .Incomplete = false },
			};
			return read;
		}

		GraphEditorTarget Target()
		{
			return GraphEditorTarget{ .Graph = AssetHandle(1), .Node = 0, .Line = 0, .JsonPointer = {}, .Finding = {} };
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("GraphEditorTarget: a target names its node directly, by a line of the text, by a JSON pointer or by an import's GRAPH_INVALID message")
		{
			const GraphReadResult read = MakeRead();
			GraphEditorTarget target = Target();
			CHECK(ResolveGraphEditorTarget(target, read) == 0);

			target.Node = 9;
			CHECK(ResolveGraphEditorTarget(target, read) == 9);
			target.Node = 5; // no such node: the other fields decide
			CHECK(ResolveGraphEditorTarget(target, read) == 0);

			// A line of the transpiled text: the node whose code it is (1-based lines); a layout line, a node the document no
			// longer holds and a line past the text name none.
			target = Target();
			target.Line = 3;
			CHECK(ResolveGraphEditorTarget(target, read) == 9);
			target.Line = 5;
			CHECK(ResolveGraphEditorTarget(target, read) == 12);
			for (const uint32_t line : { 1u, 6u, 7u, 8u })
			{
				target.Line = line;
				CHECK(ResolveGraphEditorTarget(target, read) == 0);
			}

			// A project.validate finding's pointer: the diagnostic at it, else the node "/Nodes/<i>" starts with.
			target = Target();
			target.JsonPointer = "/Nodes/2/Values/Condition";
			CHECK(ResolveGraphEditorTarget(target, read) == 12);
			target.JsonPointer = "/Nodes/1/Kind";
			CHECK(ResolveGraphEditorTarget(target, read) == 9);
			target.JsonPointer = "/Nodes/3";
			CHECK(ResolveGraphEditorTarget(target, read) == 0);
			target.JsonPointer = "/Nodes/1x";
			CHECK(ResolveGraphEditorTarget(target, read) == 0);
			target.JsonPointer = "/Name";
			CHECK(ResolveGraphEditorTarget(target, read) == 0);

			// An import check's GRAPH_INVALID finding, "<pointer>: <message>" as GraphImporter writes it.
			target = Target();
			target.Finding = "/Nodes/2/Values/Condition: the input is not linked";
			CHECK(ResolveGraphEditorTarget(target, read) == 12);
			target.Finding = "the graph has no name";
			CHECK(ResolveGraphEditorTarget(target, read) == 0);
			target.Finding = "/Nodes/0: node 4 never runs"; // GRAPH_UNREACHABLE_NODE is never an import finding
			CHECK(ResolveGraphEditorTarget(target, read) == 0);
		}

		TEST_CASE("GraphEditorTarget: a graph is a Script asset of the Graph importer, found by its source path")
		{
			Test::AutomationFixture setup("GraphEditorTargetAssets");
			REQUIRE(setup.Call("graph.create", Json{ { "path", "Assets/Scripts/Door.graph" } }));
			const Result<Json> written = setup.Call("script.write", Json{ { "path", "Assets/Scripts/Mover.luau" }, { "source", "return Script.Define(\"Mover\", {})" } });
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			EditorAssetManager& assets = setup.GetEditor().GetAssets();
			const std::optional<AssetHandle> graph = assets.Resolve("Assets/Scripts/Door.graph");
			const std::optional<AssetHandle> script = assets.Resolve("Assets/Scripts/Mover.luau");
			REQUIRE(graph.has_value());
			REQUIRE(script.has_value());
			const AssetRegistry& registry = assets.GetRegistry();
			CHECK(IsGraphAsset(registry, *graph));
			CHECK_FALSE(IsGraphAsset(registry, *script));
			CHECK_FALSE(IsGraphAsset(registry, AssetHandle(12345)));
			CHECK(FindGraphAsset(registry, "Assets/Scripts/Door.graph") == graph);
			CHECK_FALSE(FindGraphAsset(registry, "Assets/Scripts/Mover.luau").has_value());
			CHECK_FALSE(FindGraphAsset(registry, "Assets/Scripts/Missing.graph").has_value());
			CHECK_FALSE(FindGraphAsset(registry, "").has_value());
		}
	}

}
