#include "TestsPCH.h"

#include "EditorCore/Graph/GraphEditorActions.h"

#include "EditorCore/Automation/GraphMethods.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

// The graph editor's actions (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decision 15, W24.2's stream A): every canvas
// action that changes a graph submits a graph method or an edit.* method, and every graph edit method is some action's. Live:
// the contract implements the mapping and the method names.

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("GraphEditorActions: every canvas action submits a graph edit method or an edit method, and every graph edit method has its action")
		{
			std::set<std::string_view> submitted;
			for (const GraphEditorAction action : AllGraphEditorActions)
			{
				const std::string_view method = GetGraphEditorActionMethod(action);
				CAPTURE(method);
				const bool graphMethod = std::ranges::find(GraphEditMethodNames, method) != GraphEditMethodNames.end();
				const bool editMethod = method == "edit.batch" || method == "edit.undo" || method == "edit.redo";
				CHECK((graphMethod || editMethod));
				submitted.insert(method);
			}
			// Every graph method that changes a graph is an action's; graph.read and graph.nodes only read (the canvas shows them).
			for (const std::string_view method : GraphEditMethodNames)
			{
				if (method == "graph.read" || method == "graph.nodes")
					continue;
				CHECK_MESSAGE(submitted.contains(method), method);
			}
			// The content browser's New Graph is graph.create; the inspector's assigns the new graph to its slot in the same undo
			// step, so it is an edit.batch of graph.create and entity.update (decision 15).
			CHECK(GetGraphEditorActionMethod(GraphEditorAction::CreateGraph) == "graph.create");
			CHECK(GetGraphEditorActionMethod(GraphEditorAction::CreateGraphInSlot) == "edit.batch");
			CHECK(GetGraphEditorActionMethod(static_cast<GraphEditorAction>(200)).empty());
			CHECK(std::ranges::is_sorted(GraphEditMethodNames));
		}
	}

}
