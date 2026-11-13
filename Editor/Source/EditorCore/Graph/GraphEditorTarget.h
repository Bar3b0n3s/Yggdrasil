#pragma once

#include "Engine/Asset/AssetHandle.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// What the graph editor opens (M24, W24.2's stream A; Docs/Decisions/0035-m24-contract.md decision 15): a graph, and the node to
// select and frame once graph.read gives the graph. The content browser and the inspector's Open Graph name the graph alone;
// a finding in the Diagnostics panel names it the way the finding locates it (review finding 16 of C24.1: a graph finding
// opens the Graph Editor at its node, not the .graph JSON at a line), and the node is the one graph.read's diagnostics give
// (decision 13): a GRAPH_INVALID finding's own node, and a type, compile or runtime finding's through the line map of the
// graph's current text (FindGraphNodeAtLine's). UI-free and pure.

namespace Engine {

	class AssetRegistry;
	struct GraphReadResult;

	struct GraphEditorTarget
	{
		AssetHandle Graph{};
		uint32_t Node = 0;         // a node to select and frame; 0 when the fields below name it, or none
		uint32_t Line = 0;         // a line of the graph's transpiled text: a type, compile or runtime finding's
		std::string JsonPointer{}; // a project.validate finding's pointer into the .graph document (its Field)
		// An import check's GRAPH_INVALID finding's message, "<pointer>: <message>" (or the message alone without a pointer), as
		// GraphImporter writes it (IScriptDiagnosticsProvider.h's ScriptDiagnostic).
		std::string Finding{};

		bool operator==(const GraphEditorTarget&) const = default;
	};

	// The node `target` names in `read`, graph.read {luau: true}'s result for target.Graph, or 0 when it names none that the
	// document holds, tried in this order:
	//   - target.Node itself;
	//   - for target.Line, the node the line map names (read.LineNodes; none while the graph does not transpile);
	//   - for target.JsonPointer, the node of the first diagnostic at that pointer, else of the "/Nodes/<i>" the pointer starts with;
	//   - for target.Finding, the node of the first GRAPH_INVALID diagnostic the finding was written from.
	// Pure.
	[[nodiscard]] uint32_t ResolveGraphEditorTarget(const GraphEditorTarget& target, const GraphReadResult& read);

	// Whether `handle` is a graph: a Script asset that the Graph importer imports (GraphImporter::Id, a .graph source).
	[[nodiscard]] bool IsGraphAsset(const AssetRegistry& registry, AssetHandle handle);

	// The graph whose source is the project-relative `path` ("Assets/Scripts/Door.graph"), or nullopt when `path` names no
	// graph. Pure over `registry`.
	[[nodiscard]] std::optional<AssetHandle> FindGraphAsset(const AssetRegistry& registry, std::string_view path);

}
