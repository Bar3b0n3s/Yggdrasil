#include "EditorPCH.h"
#include "EditorCore/Graph/GraphEditorActions.h"

// The graph editor's actions (Docs/Decisions/0035-m24-contract.md decision 15): the contract's mapping; W24.2's stream A
// submits it from the canvas.

namespace Engine {

	std::string_view GetGraphEditorActionMethod(GraphEditorAction action)
	{
		switch (action)
		{
			case GraphEditorAction::CreateGraph:       return "graph.create";
			case GraphEditorAction::CreateGraphInSlot: return "edit.batch";
			case GraphEditorAction::AddNode:           return "graph.addNode";
			case GraphEditorAction::AddLinkedNode:     return "edit.batch";
			case GraphEditorAction::RemoveNodes:       return "graph.removeNodes";
			case GraphEditorAction::MoveNodes:         return "graph.moveNodes";
			case GraphEditorAction::Connect:           return "graph.connect";
			case GraphEditorAction::Disconnect:        return "graph.disconnect";
			case GraphEditorAction::SetValues:         return "graph.setValues";
			case GraphEditorAction::SetMembers:        return "graph.setMembers";
			case GraphEditorAction::DuplicateNodes:    return "edit.batch";
			case GraphEditorAction::Undo:              return "edit.undo";
			case GraphEditorAction::Redo:              return "edit.redo";
		}
		return {};
	}

}
