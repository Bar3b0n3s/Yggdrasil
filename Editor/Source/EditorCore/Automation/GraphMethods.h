#pragma once

#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Reflection/VariantValue.h"

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// graph.* (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decisions 12 to 14): a graph built, read and edited through
// automation alone, so an agent builds a graph through MCP only, and the editor's graph canvas acts through these methods
// alone (EditorCore/Graph/GraphEditorActions.h), so every editor graph action has its method (Roadmap rule 5).
//
// Conventions, as in MethodRegistry.h and AssetMethods.h:
//   - "graph" is an AssetReference (§7.1) of a Script asset whose importer is "Graph" (a .graph file); the result names it as
//     an AssetSummary (type Script).
//   - An edit reads the .graph source (GraphFromText, strict), applies its change to the document, keeps it canonical
//     (CanonicalizeGraph) and writes its canonical text (GraphToText) as one AssetEditCommand executed through
//     EditorContext::Execute: one undo step labelled "[agent] <label>" (each method's label below), provenance recorded by
//     the write path like every write, the result's undoIndex (0 in a dry run, in a batch and when nothing changed: nothing is
//     written then). Asset edits write through (ADR 0010): the file is reimported, and a lockstep, recording, replay or test
//     session sees the change only after it ends, as asset.setProperties' (§7.5 race rule 4).
//   - An edit is refused, writing nothing, when it would break a rule (ValidateGraphStructure's, or a ValidateGraph issue
//     that is not Incomplete); a graph may be incomplete on the way to a finished one (a required input not linked yet), which
//     graph.read's diagnostics and the import report (GRAPH_INVALID) until it is finished.
//   - Every method is AllowedInBatch, so edit.batch builds a graph atomically, its ops naming earlier ops' results with M19's
//     named "$ref" ({"$ref": "door.node"}); every edit SupportsDryRun. They edit assets, never an open scene, so they take no
//     scene target (the scene-target inventory's relation None), and none is available in the Runtime.
//   - The source of an edit that does not read (a hand-broken file) fails with ImportFailed naming the file (the hint names
//     project.validate); the reference fails as asset.setProperties' (NotFound; InvalidArgument for a built-in or a
//     sub-asset) at /graph, and InvalidArgument at /graph for an asset whose importer is not Graph (the hint names
//     script.read for a Luau script).
//
// Frozen by the M24 contract (C24.1): the wire format below. The types and the registration are written here; stream G-C
// implements the edit handlers and W24.2's stream B implements graph.liveValues, each with its C++ round trip, its Python
// test, its scene-target inventory entry and the MCP schema test's proxied tools. The lead registers them from
// RegisterMethods.cpp at each wave's integration (RegisterGraphMethodTypes and RegisterGraphMethods at W24.1's,
// RegisterGraphLiveValueMethods at W24.2's), with the regenerated references and catalogue (decision 22).

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// The most items (nodes, positions, values, members) one graph method takes.
	inline constexpr uint32_t MaxGraphMethodItems = 1024;

	// The graph methods RegisterGraphMethods registers (graph.liveValues is RegisterGraphLiveValueMethods'), in name order.
	inline constexpr std::array<std::string_view, 10> GraphEditMethodNames = { "graph.addNode", "graph.connect", "graph.create",
		"graph.disconnect", "graph.moveNodes", "graph.nodes", "graph.read", "graph.removeNodes", "graph.setMembers", "graph.setValues" };

	// --- graph.create --------------------------------------------------------------------------------------------------------

	// graph.create {path, name?}: a new, empty graph (no member, node or link) and its .meta (importer Graph, type Script), one
	// AssetEditCommand (label "Create Graph"), as script.create writes a script.
	struct GraphCreateParams
	{
		std::string Path{}; // required: a new "Assets/....graph" path
		std::string Name{}; // the behaviour's name; empty: the file's stem when IsGraphIdentifier, else refused at /name
	};

	struct GraphCreateResult
	{
		AssetSummary Graph{};
		std::string Name{};
		uint32_t UndoIndex = 0;
	};

	// --- graph.read ----------------------------------------------------------------------------------------------------------

	// One finding of a graph: a ValidateGraph issue (GRAPH_INVALID), a node no event reaches (GRAPH_UNREACHABLE_NODE), or a
	// finding of the latest import's script check of its transpiled text (SCRIPT_TYPE_ERROR, SCRIPT_COMPILE_ERROR) mapped to
	// its node (decision 8).
	struct GraphDiagnostic
	{
		DiagnosticSeverity Severity = DiagnosticSeverity::Error;
		std::string Code{};
		std::string Message{};
		uint32_t Node = 0;         // 0: the graph's members or name, or a line no node owns
		std::string JsonPointer{}; // into the .graph document; empty for a finding of the text
		uint32_t Line = 0;         // the transpiled text's line of a script check's finding; 0 otherwise
		bool Incomplete = false;   // GraphIssue::Incomplete
	};

	// graph.read {graph, luau?}: the document as the file holds it, its findings, and with luau its transpiled text and line
	// map (empty while the graph does not transpile).
	struct GraphReadParams
	{
		std::string Graph{}; // required
		bool Luau = false;
	};

	struct GraphReadResult
	{
		AssetSummary Graph{};
		GraphDocument Document{};                   // registry struct "Graph", in the file's spelling
		std::vector<GraphDiagnostic> Diagnostics{}; // ValidateGraph's in document order, then the unreachable nodes, then the script check's by line
		std::string Luau{};                         // with luau: TranspileGraph's text
		std::vector<uint32_t> LineNodes{};          // with luau: its line map
	};

	// --- graph.nodes ---------------------------------------------------------------------------------------------------------

	// graph.nodes {graph?, prefix?, kinds?}: the node library (GraphNodeLibrary::GetNodes) for the canvas's menu and for an agent
	// choosing kinds, with graph its own kinds too (GraphNodeLibrary::GetMemberNodes: Self.Get and Self.Set, and M39's Method and
	// Return of its methods), filtered by a kind prefix and an exact list.
	struct GraphNodesParams
	{
		std::string Graph{};              // empty: the graph-independent kinds alone
		std::string Prefix{};             // only kinds starting with it ("Call.Input.", "Flow.")
		std::vector<std::string> Kinds{}; // only these kinds (each unknown one refused at /kinds/<i>, with suggestions)
	};

	struct GraphNodesResult
	{
		std::vector<GraphNodeDescription> Nodes{}; // registry struct "GraphNodeInfo", sorted by kind
	};

	// --- graph.addNode -------------------------------------------------------------------------------------------------------

	// graph.addNode {graph, kind, position?, values?} (label "Add Graph Node"): a node of `kind` with id GetNextGraphNodeId,
	// at `position` (absent: one node width to the right of the rightmost node, at the top of the graph's bounds; (0, 0) for
	// the first), with `values` as its input literals.
	struct GraphAddNodeParams
	{
		std::string Graph{}; // required
		std::string Kind{};  // required: a kind GraphNodeLibrary::Resolve finds for the graph
		glm::vec2 Position = glm::vec2(0.0f);
		std::map<std::string, VariantValue> Values{};
	};

	struct GraphAddNodeResult
	{
		AssetSummary Graph{};
		uint32_t Node = 0; // the new node's id
		uint32_t UndoIndex = 0;
	};

	// --- graph.removeNodes ---------------------------------------------------------------------------------------------------

	// graph.removeNodes {graph, nodes} (label "Remove Graph Nodes"): the nodes and every link to or from them.
	struct GraphRemoveNodesParams
	{
		std::string Graph{};           // required
		std::vector<uint32_t> Nodes{}; // required: 1 to MaxGraphMethodItems ids of the graph's nodes
	};

	struct GraphRemoveNodesResult
	{
		AssetSummary Graph{};
		std::vector<GraphLink> RemovedLinks{}; // in the canonical order
		uint32_t UndoIndex = 0;
	};

	// --- graph.moveNodes -----------------------------------------------------------------------------------------------------

	// Registry struct "GraphNodePosition".
	struct GraphNodePosition
	{
		uint32_t Node = 0;
		glm::vec2 Position = glm::vec2(0.0f);
	};

	// graph.moveNodes {graph, positions} (label "Move Graph Nodes"): layout only, which the transpiled text never reads.
	struct GraphMoveNodesParams
	{
		std::string Graph{};                        // required
		std::vector<GraphNodePosition> Positions{}; // required: 1 to MaxGraphMethodItems, a later one of a node winning
	};

	// The result of the edits that report nothing else (graph.moveNodes, graph.disconnect, graph.setValues).
	struct GraphEditResult
	{
		AssetSummary Graph{};
		uint32_t UndoIndex = 0;
	};

	// --- graph.connect and graph.disconnect ----------------------------------------------------------------------------------

	// graph.connect {graph, from, to} (label "Connect Graph Pins"): a link from an output pin to an input pin. A data input
	// that holds a link, and an exec output that holds one, lose it to the new link (Replaced). Refused: an unknown node or
	// pin (InvalidArgument at /from/Node, /from/Pin, /to/Node or /to/Pin, the members of the wire's GraphPinRef, with the
	// pin's suggestions), the wrong directions, the same node at both ends, incompatible pins (AreGraphPinsCompatible; at /to,
	// naming both types; an optional output into a required input with the hint naming Flow.IfNotNil) and a link that would
	// close a cycle (at /to).
	struct GraphConnectParams
	{
		std::string Graph{}; // required
		GraphPinRef From{};  // required: an output pin
		GraphPinRef To{};    // required: an input pin
	};

	struct GraphConnectResult
	{
		AssetSummary Graph{};
		std::vector<GraphLink> Replaced{}; // the links the new one replaced, at most one
		uint32_t UndoIndex = 0;
	};

	// graph.disconnect {graph, from, to} (label "Disconnect Graph Pins"): removes the link; NotFound at /to when there is none.
	struct GraphDisconnectParams
	{
		std::string Graph{}; // required
		GraphPinRef From{};  // required
		GraphPinRef To{};    // required
	};

	// --- graph.setValues -----------------------------------------------------------------------------------------------------

	// Registry struct "GraphValueEdit": one input literal; a null value removes the literal.
	struct GraphValueEdit
	{
		uint32_t Node = 0;
		std::string Pin{};
		VariantValue Value{};
	};

	// graph.setValues {graph, values} (label "Set Graph Values"): input literals, in order (a later one of a pin winning). A null
	// value removes a literal the node holds, also on a pin its kind no longer has (a binding's parameter renamed or removed),
	// which leaves the graph GRAPH_INVALID until it goes (M24's review). Refused at /values/<i>/node, /values/<i>/pin or
	// /values/<i>/value: an unknown node, a pin that is not a data input of its node (but for that removal), a value its pin's
	// type does not take (ValidateGraph's literal rule).
	struct GraphSetValuesParams
	{
		std::string Graph{};                  // required
		std::vector<GraphValueEdit> Values{}; // required: 1 to MaxGraphMethodItems
	};

	// --- graph.setMembers ----------------------------------------------------------------------------------------------------

	// Registry struct "GraphMemberRename": a field, a variable or (M39) a method to rename.
	struct GraphMemberRename
	{
		std::string From{};
		std::string To{};
	};

	// graph.setMembers {graph, name?, rename?, remove?, fields?, variables?, methods?, runsWhileWorldPaused?} (label "Set Graph
	// Members"), applied in that order: the behaviour's name; renames of fields, variables and methods, each rewriting its
	// nodes' kinds (Self.Get and Self.Set of a field or variable, Method and Return of a method); removals, refused at
	// /remove/<i> while a node uses the member (its hint names graph.removeNodes and the node ids); then each field, variable
	// and method given, added or replacing the member of its name (a member may not change between a field, a variable and a
	// method but by its removal and addition, and a replaced member's kind change is refused at /fields/<i>/Kind or
	// /variables/<i>/Kind, a method's signature change at /methods/<i>/Parameters or /methods/<i>/Results, while a node's link
	// or literal needs the old one); then the world-pause opt-in. M39 (Docs/Decisions/0056-m39-graphs-behaviours.md
	// decision 8 and departure 1): `methods` are free-form values, so the MCP tool's compact schema stays small, each read
	// strictly through the registry's "GraphMethod" struct and refused at /methods/<i>/... when it is not one. A method's
	// callers name it by a string (Script.Call's Method, Luau's ":Name()"), which no rename rewrites.
	struct GraphSetMembersParams
	{
		std::string Graph{}; // required
		std::string Name{};  // empty: unchanged
		std::vector<GraphMemberRename> Rename{};
		std::vector<std::string> Remove{};
		std::vector<GraphField> Fields{};
		std::vector<GraphVariable> Variables{};
		std::vector<VariantValue> Methods{}; // M39: GraphMethod values
		bool RunsWhileWorldPaused = false;   // M39: applied only when the param is given
	};

	struct GraphSetMembersResult
	{
		AssetSummary Graph{};
		uint32_t RenamedNodes = 0; // the Self.Get, Self.Set, Method and Return nodes the renames rewrote
		uint32_t UndoIndex = 0;
	};

	// --- graph.liveValues (W24.2, stream B) ----------------------------------------------------------------------------------

	// graph.liveValues {entity, script?}: during play, the live values of the graph behaviour on an entity of the play scene,
	// read between frames (ScriptEngine::ReadInstanceValues) without changing the session's state: its nodes' last values
	// (the live-value table's keys parsed with ParseGraphProbeKey), its members' values, and the session's retained script
	// errors of that instance raised by the graph's own code, mapped to their nodes through the graph's current transpiled
	// text (decision 14). Admitted while a test run owns the editor (W24.2's integration), as graph.read and graph.nodes are
	// (W24.2's review). The graph's read and its text's line map are kept between calls until the graph changes
	// (EditorContext::GetGraphLiveCache).
	struct GraphLiveValuesParams
	{
		std::string Entity{}; // required: an entity of the play scene (its id, a unique id prefix or its path)
		// The slot's behaviour name, its graph's current Name (of two such slots, the first); empty: the entity's first slot
		// holding a graph.
		std::string Script{};
	};

	// Registry struct "GraphLiveValue": one node output's last value.
	struct GraphLiveValue
	{
		uint32_t Node = 0;
		std::string Pin{};
		std::string Type{}; // ScriptInstanceValue::TypeName
		VariantValue Value{};
	};

	// Registry struct "GraphLiveMember": one field's or variable's value.
	struct GraphLiveMember
	{
		std::string Name{};
		std::string Type{};
		VariantValue Value{};
	};

	// Registry struct "GraphLiveError": one retained script error of the instance.
	struct GraphLiveError
	{
		uint32_t Node = 0; // 0 when the line maps to no node
		uint32_t Line = 0;
		std::string Message{};
		uint32_t Count = 0;
	};

	struct GraphLiveValuesResult
	{
		AssetSummary Graph{};
		EntitySummary Entity{};
		uint32_t Slot = 0;
		uint32_t Tick = 0;                      // the play session's tick (ToAutomationCounter)
		std::vector<GraphLiveValue> Values{};   // sorted by (node, pin)
		std::vector<GraphLiveMember> Members{}; // the graph's fields and variables, by name
		std::vector<GraphLiveError> Errors{};
		bool Truncated = false; // ScriptInstanceValues::Truncated
	};

	namespace Automation {

		// graph.create. Errors: InvalidArgument at /path for a path outside Assets/, without the .graph extension or naming an
		// existing file; at /name for a name that is not IsGraphIdentifier; the AssetEditCommand's errors.
		[[nodiscard]] Result<GraphCreateResult> GraphCreate(EditorMethodContext& context, const GraphCreateParams& params);
		// graph.read. Errors: the reference errors of the file comment; ImportFailed for a source that does not read.
		[[nodiscard]] Result<GraphReadResult> GraphRead(EditorMethodContext& context, const GraphReadParams& params);
		// graph.nodes. Errors: the reference errors for a given graph; InvalidArgument at /kinds/<i> for an unknown kind.
		[[nodiscard]] Result<GraphNodesResult> GraphNodes(EditorMethodContext& context, const GraphNodesParams& params);
		// graph.addNode. Errors: InvalidArgument at /kind for a kind the library does not resolve (its suggestions, the hint
		// naming graph.nodes), at /position for a position outside MaxGraphPosition, at /values/<pin> as graph.setValues'; Validation
		// when the graph holds MaxGraphNodes nodes or GetNextGraphNodeId gives 0.
		[[nodiscard]] Result<GraphAddNodeResult> GraphAddNode(EditorMethodContext& context, const GraphAddNodeParams& params);
		// graph.removeNodes. Errors: InvalidArgument at /nodes for none or more than MaxGraphMethodItems, at /nodes/<i> for an
		// unknown or repeated id, and at /nodes for a removal that adds a ValidateGraph issue that is not Incomplete (removing one
		// of the exec links into a merge lays its flow out again at the other link's depth, which may nest it too deep).
		[[nodiscard]] Result<GraphRemoveNodesResult> GraphRemoveNodes(EditorMethodContext& context, const GraphRemoveNodesParams& params);
		// graph.moveNodes. Errors: InvalidArgument at /positions for none or too many, at /positions/<i>/node for an unknown id,
		// at /positions/<i>/position for a position outside MaxGraphPosition.
		[[nodiscard]] Result<GraphEditResult> GraphMoveNodes(EditorMethodContext& context, const GraphMoveNodesParams& params);
		// graph.connect (its refusals above).
		[[nodiscard]] Result<GraphConnectResult> GraphConnect(EditorMethodContext& context, const GraphConnectParams& params);
		// graph.disconnect. Errors: NotFound at /to for a link the graph does not hold; InvalidArgument at /to for a removal that
		// adds a ValidateGraph issue that is not Incomplete (as graph.removeNodes' merge).
		[[nodiscard]] Result<GraphEditResult> GraphDisconnect(EditorMethodContext& context, const GraphDisconnectParams& params);
		// graph.setValues (its refusals above).
		[[nodiscard]] Result<GraphEditResult> GraphSetValues(EditorMethodContext& context, const GraphSetValuesParams& params);
		// graph.setMembers (its refusals above, and ValidateGraphStructure's for a member, located in the params).
		[[nodiscard]] Result<GraphSetMembersResult> GraphSetMembers(EditorMethodContext& context, const GraphSetMembersParams& params);
		// graph.liveValues. Errors: InvalidState without a play session (the hint names play.start) and in a simulation, which
		// runs no behaviours; NotFound at /entity for an entity the play scene does not hold (InvalidArgument for an ambiguous
		// or malformed reference); NotFound at /script for an entity with no graph slot, or none whose behaviour is `script`
		// (a graph source that does not read names none); without `script`, the first graph slot's read errors (ImportFailed for
		// a source that does not read); ReadInstanceValues' errors.
		[[nodiscard]] Result<GraphLiveValuesResult> GraphLiveValues(EditorMethodContext& context, const GraphLiveValuesParams& params);

	}

	// Registers the structs and enums above, and the library's (GraphPinType, GraphPinDirection, GraphNodeCategory,
	// "GraphPinInfo", "GraphNodeInfo"). The registry must hold RegisterGraphTypes' types first.
	void RegisterGraphMethodTypes(TypeRegistry& registry);

	// Registers GraphEditMethodNames' methods: graph.read and graph.nodes read-only; the others Mutates and SupportsDryRun; all
	// AllowedInBatch; every one an MCP tool but graph.moveNodes (layout, reachable through engine_call); none available in the
	// Runtime.
	void RegisterGraphMethods(MethodRegistry& methods);

	// Registers graph.liveValues (W24.2, stream B): read-only, AllowedInBatch, an MCP tool; not available in the Runtime.
	void RegisterGraphLiveValueMethods(MethodRegistry& methods);

}
