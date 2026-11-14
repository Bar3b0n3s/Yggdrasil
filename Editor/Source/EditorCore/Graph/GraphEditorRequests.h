#pragma once

#include "EditorCore/Automation/GraphMethods.h"
#include "EditorCore/Graph/GraphEditorActions.h"
#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <glm/vec2.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// The requests the graph canvas submits (M24, W24.2's stream A; Docs/Decisions/0035-m24-contract.md decision 15): for each
// GraphEditorAction that changes a graph, the method GetGraphEditorActionMethod names with its params, built UI-free so the
// canvas's actions are tested headless against the methods themselves. A method's own params are its param struct (GraphMethods.h)
// written through the editor's type registry, which holds the graph methods' types (RegisterGraphMethodTypes), so they are the
// wire's spelling by construction; the actions that compose several edits (AddLinkedNode, DuplicateNodes, MoveLink, and a
// RemoveNodes or MoveNodes of more nodes than one call takes) are one edit.batch, whose later ops name the nodes the earlier
// ones add with M19's named "$ref" ({"$ref": "n4.node"}), so each is one undo step and a refused op undoes the ones before it.
// The canvas submits every request through the editor's queued actions (EditorActions), as automation's in-process client.

namespace Engine {

	class TypeRegistry;

	// One request: the action, the method it submits and that method's params.
	struct GraphEditorRequest
	{
		GraphEditorAction Action = GraphEditorAction::AddNode;
		std::string Method{}; // GetGraphEditorActionMethod(Action); edit.batch for a RemoveNodes or MoveNodes split into calls
		Json Params{};
	};

	// The most ops one edit.batch runs (edit.batch's own limit, EditMethods.h); a duplicate or paste of more nodes and links is
	// refused before it is submitted.
	inline constexpr size_t MaxGraphEditorBatchOps = 1000;

	// The request of each single-method action, its params `params` (the method's param struct). RemoveNodes and MoveNodes of
	// more nodes or positions than one call takes (MaxGraphMethodItems; a box selection of a large graph) are one edit.batch
	// (labelled "Remove Graph Nodes" or "Move Graph Nodes") of calls of that method, each of at most that many in order, so the
	// edit is one undo step, as one call is (M24's review). Errors: InvalidState when `types` lacks the graph methods' types;
	// the struct's ToJson errors (Validation for a non-finite position); InvalidArgument for more items than one edit.batch of
	// such calls takes (MaxGraphEditorBatchOps calls).
	[[nodiscard]] Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphCreateParams& params);      // CreateGraph
	[[nodiscard]] Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphAddNodeParams& params);     // AddNode
	[[nodiscard]] Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphRemoveNodesParams& params); // RemoveNodes
	[[nodiscard]] Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphMoveNodesParams& params);   // MoveNodes
	[[nodiscard]] Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphConnectParams& params);     // Connect
	[[nodiscard]] Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphDisconnectParams& params);  // Disconnect
	[[nodiscard]] Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphSetValuesParams& params);   // SetValues
	[[nodiscard]] Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphSetMembersParams& params);  // SetMembers

	// Undo and Redo: edit.undo and edit.redo of one step.
	[[nodiscard]] GraphEditorRequest MakeGraphUndoRequest();
	[[nodiscard]] GraphEditorRequest MakeGraphRedoRequest();

	// AddLinkedNode: a link dropped on empty canvas from the pin `existing` (of direction `direction`), then the kind of `node`
	// chosen: one edit.batch (label "Add Linked Graph Node") of graph.addNode (`node`) and graph.connect joining `existing` and the
	// new node's pin `pin`, from the output to the input. Errors: as MakeGraphEditorRequest's.
	[[nodiscard]] Result<GraphEditorRequest> MakeGraphLinkedNodeRequest(const TypeRegistry& types, const GraphAddNodeParams& node, const GraphPinRef& existing,
		GraphPinDirection direction, std::string_view pin);

	// MoveLink: the link `link` of the graph `graph` dragged off its input and dropped on the input `to`: one edit.batch (label
	// "Move Graph Link") of graph.disconnect (`link`) and graph.connect (`link`'s output to `to`), so the move is one undo step
	// and a refused connect (incompatible pins, a cycle) leaves the link where it was. Errors: as MakeGraphEditorRequest's.
	[[nodiscard]] Result<GraphEditorRequest> MakeGraphMoveLinkRequest(const TypeRegistry& types, std::string_view graph, const GraphLink& link, const GraphPinRef& to);

	// The nodes of `graph` whose ids `nodes` lists, with their kinds, positions and literals, and the links of `graph` between
	// two of them: what Duplicate copies and what Copy keeps for Paste. Unknown ids are skipped. Pure.
	[[nodiscard]] GraphDocument CopyGraphNodes(const GraphDocument& graph, std::span<const uint32_t> nodes);

	// DuplicateNodes (Ctrl+D, and Paste of what CopyGraphNodes kept): one edit.batch (label "Duplicate Graph Nodes", or "Paste
	// Graph Nodes" with `paste`) adding to the graph `graph` a node per node of `copied`, of its kind and literals, `offset` from
	// its position (clamped to MaxGraphPosition), then connecting the new nodes as `copied`'s links connect the old ones. Errors:
	// InvalidArgument when `copied` has no node, or more nodes and links than MaxGraphEditorBatchOps; as MakeGraphEditorRequest's.
	[[nodiscard]] Result<GraphEditorRequest> MakeGraphDuplicateRequest(const TypeRegistry& types, std::string_view graph, const GraphDocument& copied,
		const glm::vec2& offset, bool paste);

}
