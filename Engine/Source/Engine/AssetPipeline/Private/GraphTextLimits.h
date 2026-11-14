#pragma once

#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/Core/Base.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// How deep and how large a graph's transpiled text may grow (M24, W24.1; Docs/Decisions/0035-m24-contract.md decision 7 and
// risk 1): the limits that TranspileGraph and ValidateGraph share, so that a graph transpiles exactly when it validates.

namespace Engine {

	namespace Utils {

		// The deepest block a statement's code may start in. Luau's parser recurses once per nested block (and its analysis
		// several times), so a text nested much deeper overflows an import worker's stack in an unoptimized build before the
		// parser's own limit (1000) refuses it; flows that deep are generated, never written, and a graph keeps them flat with
		// Flow.Sequence and merges.
		inline constexpr size_t MaxGraphTextDepth = 64;

		// The most bytes a graph's transpiled text may hold (8 MiB). The text grows with every statement times the pure
		// values it reads, each evaluated again in the statement's own block (decision 7), so the document's limits alone
		// bound it only at hundreds of megabytes; this one keeps every layout (each validation, the import's, the edits') to a
		// few megabytes. A real graph's text is a few kilobytes (FeatureTest's and the fixture graphs' at most 5 KB, the tests'
		// graphs at MaxGraphNodes' scale at most 0.45 MB), and the most a graph's fields, variables and layout lines can give
		// without a statement is about 4.4 MB, so the text passes the limit only inside a statement's code.
		inline constexpr size_t MaxGraphTextBytes = 8 * 1024 * 1024;

		// What laying `graph`'s text out as TranspileGraph lays it out finds against the two limits, so it is that text's
		// depth and size exactly. `graph` passes ValidateGraphStructure and every ValidateGraph rule but the Incomplete ones.
		struct GraphTextLimitIssues
		{
			// The statements (indexes into graph.Nodes, sorted) whose code would start deeper than MaxGraphTextDepth blocks: on
			// each flow that goes too deep, the statement that first passes the limit, the deepest one laid out (the layout
			// never goes past it).
			std::vector<size_t> TooDeep{};
			// The statement (an index into graph.Nodes) whose code the text passes MaxGraphTextBytes in, where the layout
			// stops; none while the text stays within the limit.
			std::optional<size_t> TooLarge{};
		};

		// Lays `graph`'s text out against `library` and reports where it passes a limit. Pure.
		[[nodiscard]] GraphTextLimitIssues FindGraphTextLimitIssues(const GraphDocument& graph, const GraphNodeLibrary& library);

		// The message of GraphTextLimitIssues::TooDeep's `node`, naming its id and kind.
		[[nodiscard]] std::string DescribeGraphStatementTooDeep(const GraphNode& node);

		// The message of GraphTextLimitIssues::TooLarge's `node`, naming its id and kind.
		[[nodiscard]] std::string DescribeGraphTextTooLarge(const GraphNode& node);

	}

}
