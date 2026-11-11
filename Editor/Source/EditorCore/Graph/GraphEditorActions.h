#pragma once

#include "Engine/Core/Base.h"

#include <array>
#include <cstdint>
#include <string_view>

// The graph editor's actions (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decisions 15 and 16; W24.2's stream A):
// every action of the graph canvas (Editor/Panels/GraphEditorPanel) that changes a graph submits its automation method, and
// only it, so the editor edits a graph exactly as automation does and every editor graph action has its method (Roadmap
// rule 5), as the animation preview's controls submit animation.preview. The canvas's presentation is exempt by decision 16,
// never silently: opening and closing a graph in the canvas, panning, zooming, framing, hovering and selecting nodes and
// links, and the canvas window's own layout change no graph and have no method. UI-free, so the mapping is tested headless.

namespace Engine {

	// What the canvas does to a graph.
	enum class GraphEditorAction : uint8_t
	{
		CreateGraph,       // the content browser's New Graph: graph.create
		CreateGraphInSlot, // the inspector's New Graph in a script slot's picker: graph.create and the slot's entity.update ops in one edit.batch
		AddNode,           // the node menu: graph.addNode
		AddLinkedNode,     // a link dropped on empty canvas, then a kind chosen: graph.addNode and graph.connect ops in one edit.batch
		RemoveNodes,       // Delete on the selected nodes: graph.removeNodes
		MoveNodes,         // the end of a drag of the selected nodes: graph.moveNodes
		Connect,           // a link dragged from a pin to a pin: graph.connect
		Disconnect,        // a link removed (Alt+click, or dragged off its input): graph.disconnect
		SetValues,         // an input literal's field: graph.setValues
		SetMembers,        // the members side panel (name, fields, variables): graph.setMembers
		DuplicateNodes,    // Ctrl+D and paste: graph.addNode and graph.connect ops in one edit.batch
		Undo,              // Ctrl+Z: edit.undo
		Redo               // Ctrl+Y: edit.redo
	};

	// Every action, in enumeration order.
	inline constexpr std::array<GraphEditorAction, 13> AllGraphEditorActions = { GraphEditorAction::CreateGraph, GraphEditorAction::CreateGraphInSlot,
		GraphEditorAction::AddNode, GraphEditorAction::AddLinkedNode, GraphEditorAction::RemoveNodes, GraphEditorAction::MoveNodes, GraphEditorAction::Connect,
		GraphEditorAction::Disconnect, GraphEditorAction::SetValues, GraphEditorAction::SetMembers, GraphEditorAction::DuplicateNodes,
		GraphEditorAction::Undo, GraphEditorAction::Redo };

	// The method an action submits ("graph.addNode", "edit.batch", "edit.undo"); empty for a value outside the enumeration.
	// Pure.
	[[nodiscard]] std::string_view GetGraphEditorActionMethod(GraphEditorAction action);

}
