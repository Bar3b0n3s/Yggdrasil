#pragma once

#include "EditorCore/Automation/GraphMethods.h"
#include "EditorCore/Graph/GraphCanvas.h"
#include "EditorCore/Graph/GraphEditorRequests.h"
#include "EditorCore/Graph/GraphEditorTarget.h"
#include "EditorCore/Graph/GraphLiveView.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <glm/vec2.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	class EditorContext;
	struct EditorPanelContext;

	// The graph editor (M24, W24.2's stream A; Docs/Decisions/0035-m24-contract.md decision 15): a first-party canvas on Dear
	// ImGui in a window of its own, "Graph Editor", which EditorLayer draws while a graph is open in it (one graph at a time, as
	// M23's Animation Preview), not an EditorPanel value, so editor.state and the saved panel list do not change. The content
	// browser's double-click, the inspector's Open Graph and a graph finding of the Diagnostics panel open it (Open), the last
	// at the finding's node, selected and framed, whether or not its graph is already open.
	//
	// It shows the graph as graph.read gives it (nodes as rounded boxes with their pins, Bezier links, a grid, each node's
	// findings as its outline) with the kinds graph.nodes lists for it, and a side panel of the behaviour's members (name,
	// fields, variables, and M39's world-pause opt-in and methods with their parameters and results:
	// Docs/Decisions/0056-m39-graphs-behaviours.md decision 13) and of the selected node's input literals, a field per pin
	// type. Every action that changes the graph submits its method through the editor's queued actions, exactly as
	// automation's in-process client does (EditorCore/Graph/GraphEditorActions.h, GraphEditorRequests.h): the node menu
	// (right-click, or a link dropped on empty canvas) graph.addNode or an edit.batch with graph.connect, Delete
	// graph.removeNodes, the end of a drag graph.moveNodes, a link dragged from a pin to a pin graph.connect, Alt+click on
	// a link (or a link dragged off its input onto empty canvas) graph.disconnect, a link dragged off its input onto
	// another input an edit.batch of graph.disconnect and graph.connect (one undo step, which a refused connect leaves
	// undone), the literal fields graph.setValues, the members panel graph.setMembers, Ctrl+D and paste an edit.batch of
	// graph.addNode and graph.connect, and Undo and Redo edit.undo and edit.redo (Ctrl+Z and Ctrl+Y are the editor's own,
	// the same methods). A link or literal on a pin the node's kind no longer has (a binding's parameter renamed or
	// removed, decision 10) shows as a pin of its own in the error colour, its tooltip saying why, and goes as any link or
	// literal does (graph.disconnect, or the side panel's Clear literal through graph.setValues with null; M24's review); a
	// field being edited keeps its text or drag across a read that arrives meanwhile. Its presentation (opening and
	// closing, panning with the middle or right button, zooming with the wheel, framing with F, hovering and selecting by
	// click or box, Ctrl+C) changes no graph and has no method (decision 16's exemption).
	//
	// Live values (decision 14, W24.2's integration of stream B's GraphLiveView): while a play session that runs behaviours
	// plays (a test run's included: graph.read, graph.nodes and graph.liveValues are among the reads a test run admits), the
	// window polls graph.liveValues once a frame for the play entity selected, or else the first, holding the graph
	// (FindGraphLiveTarget), read at the editor's next safe point between frames, so showing them never changes the session.
	// Under each node it draws the node's live lines (GetNodeLiveLines: its outputs' last values, then the errors its code
	// raised, which also outline the node as an error), and beside each member in the side panel the member's value. The
	// method names the slot by its behaviour name and reads the first slot of that name, so when an earlier slot of the entity
	// holds another graph of the open graph's name, the read is that graph's: the window shows none of it and says why.
	//
	// Main thread inside ImGui; keeps only presentation state and copies of what the methods returned, never an asset pointer.
	// It reads again after every change of the command history or of the graph's asset version, so undo, redo and an agent's
	// edits show at once.
	class GraphEditorPanel
	{
	public:
		// How the canvas outlines a node (GetNodeOutline), the first that holds: Selected; Error for a finding that is an error,
		// or a live error its code raised; Warning for an unfinished input or a warning; None.
		enum class NodeOutline : uint8_t
		{
			None,
			Warning,
			Error,
			Selected
		};

		// Opens `target`'s graph, replacing the open one and its selection; the node the target names is selected and framed once
		// a graph.read asked since then gives the graph (ResolveGraphEditorTarget), so a line target finds its node even in the
		// graph already open. A null graph closes the editor.
		void Open(const GraphEditorTarget& target);
		void Close();
		[[nodiscard]] Status Draw(EditorPanelContext& context);

		// The open graph; null when the editor is closed.
		[[nodiscard]] AssetHandle GetGraph() const { return m_Graph; }
		// The selected nodes, sorted by id.
		[[nodiscard]] std::span<const uint32_t> GetSelection() const { return m_Selection; }
		[[nodiscard]] const GraphCanvasView& GetView() const { return m_View; }
		// Where the last frame drew pin `pin`'s centre, or node `node`'s box (its corners), in screen pixels; nullopt for what it
		// did not draw. For overlays drawn over the canvas.
		[[nodiscard]] std::optional<glm::vec2> GetPinScreenPosition(const GraphPinRef& pin) const;
		[[nodiscard]] std::optional<std::pair<glm::vec2, glm::vec2>> GetNodeScreenRect(uint32_t node) const;
		// The live values the window polls (decision 14).
		[[nodiscard]] const GraphLiveView& GetLiveView() const { return m_Live; }
		// The live lines the canvas draws under `node`: the view's GetNodeLines while its read is of the open graph, else none.
		[[nodiscard]] std::vector<std::string> GetNodeLiveLines(uint32_t node) const;
		[[nodiscard]] NodeOutline GetNodeOutline(uint32_t node) const;
	private:
		enum class CanvasDrag : uint8_t
		{
			None,
			Move, // the selected nodes follow the mouse
			Box,  // a selection rectangle
			Link  // a link from m_LinkFrom (or m_Detached's output) follows the mouse
		};

		// A literal or member default being edited: its candidate until the field is released and the next read shows it. A
		// read that arrives while its field is still active keeps it (M24's review), so the text typed or the drag in progress
		// survives and its release still submits it; a read after its submission, or after the field let go without one, drops
		// it, and the field shows the stored value.
		struct FieldEdit
		{
			std::string Key{};
			Json Value{};
			bool Active = false;    // the field's widget was active in the last frame that drew it
			bool Submitted = false; // the field returned its value for its method (DrawValueField)
		};

		void Reset();
		void PollReads(EditorPanelContext& context);
		void PollEdits(EditorPanelContext& context);
		void DetectChanges(EditorContext& editor);
		void Submit(EditorPanelContext& context, Result<GraphEditorRequest> request);
		void SubmitRead(EditorPanelContext& context);
		void SubmitNodes(EditorPanelContext& context);
		void ApplyRead(GraphReadResult read);
		void RebuildLayouts();
		[[nodiscard]] const GraphNodeDescription* FindDescription(std::string_view kind) const;
		[[nodiscard]] const GraphPinDescription* FindPinDescription(const GraphPinRef& pin) const;
		void Select(std::vector<uint32_t> nodes);
		// The live values of the open graph: the view's values when its read is of this graph, else null.
		[[nodiscard]] const GraphLiveValuesResult* FindShownLiveValues() const;
		void Frame(std::span<const uint32_t> nodes, const glm::vec2& canvasSize);

		void DrawToolbar(EditorPanelContext& context);
		void DrawCanvas(EditorPanelContext& context);
		void HandleCanvasInput(EditorPanelContext& context, const glm::vec2& canvasMinimum, bool hovered);
		void FinishDrag(EditorPanelContext& context, const glm::vec2& mouse);
		void HandleShortcuts(EditorPanelContext& context);
		void DrawSidePanel(EditorPanelContext& context);
		void DrawMembers(EditorPanelContext& context);
		void DrawFieldOptions(EditorPanelContext& context, const GraphField& field);
		void DrawMethod(EditorPanelContext& context, const GraphMethod& method); // M39
		void DrawNodeDetails(EditorPanelContext& context);
		void DrawNodeMenu(EditorPanelContext& context);
		void DrawContextMenu(EditorPanelContext& context);
		// The field editing the value `stored` (a literal or a member's default) with the widget `widget` (GraphEditorPanel.cpp's
		// GraphValueWidget), keyed by `key` and showing `label` beside it: `choices` an Enum's, `assetType` an Asset's type name
		// (empty: any). Returns the value to submit on the frame the field commits it (a click, a choice, a drag or a text
		// released), else nullopt; the candidate shows until the next read.
		[[nodiscard]] std::optional<Json> DrawValueField(EditorPanelContext& context, const std::string& key, std::string_view label, uint8_t widget,
			const Json& stored, std::span<const std::string> choices, std::string_view assetType);

		// Requests of the actions (each submits its method).
		void AddNode(EditorPanelContext& context, const GraphNodeDescription& node, const std::string& pin);
		void RemoveSelection(EditorPanelContext& context);
		void DuplicateNodes(EditorPanelContext& context, const GraphDocument& copied, const glm::vec2& offset, bool paste);
		void SetLiteral(EditorPanelContext& context, uint32_t node, const std::string& pin, const Json& value);
	private:
		AssetHandle m_Graph{};
		std::string m_Reference{}; // the graph's id, every request's "graph"
		std::filesystem::path m_Project{};
		std::optional<GraphEditorTarget> m_Target{}; // a node to select and frame once a read asked since it was set arrives
		bool m_TargetRead = false;                   // m_Read was asked after m_Target was set, so the target resolves against it
		bool m_FocusWindow = false;

		// What the methods returned, and their pending requests.
		std::optional<GraphReadResult> m_Read{};
		std::string m_ReadError{};
		uint64_t m_ReadTicket = 0;
		bool m_ReadStale = true;
		std::vector<GraphNodeDescription> m_Library{}; // graph.nodes {graph}: the kinds, sorted by kind
		std::string m_LibraryMembers{};                // the members the library's Self kinds were listed for
		uint64_t m_NodesTicket = 0;
		bool m_NodesStale = true;
		std::vector<uint64_t> m_EditTickets{};    // kept across Close, so an edit's refusal is still reported
		std::vector<uint64_t> m_AbandonedReads{}; // reads of a graph no longer open, discarded at the next frame
		std::string m_Error{};

		// Change signals (constant-time reads of the editor's state).
		bool m_HasSignals = false;
		uint64_t m_HistorySequence = 0;
		size_t m_HistoryUndoCount = 0;
		size_t m_HistoryRedoCount = 0;
		uint64_t m_AssetVersion = 0;
		uint64_t m_RegistryRevision = 0;

		// Live values in play (decision 14), of the graph m_LiveGraph; another graph, or none, stops them at the next Draw.
		GraphLiveView m_Live{};
		AssetHandle m_LiveGraph{};

		// The canvas.
		GraphCanvasView m_View{};
		bool m_ViewFramed = false;                   // the first read frames the whole graph
		std::vector<GraphNodeShape> m_Shapes{};      // in draw order: by id, the selected nodes last (on top)
		std::vector<GraphNodeLayout> m_Layouts{};    // m_Shapes' layouts, in graph units
		std::map<uint32_t, size_t> m_NodeFindings{}; // node -> its worst finding: 2 an error, 1 an incomplete input or a warning
		std::set<GraphPinRef> m_LinkedPins{};
		// M24's review: the pins a link or literal names that its node's kind no longer has (a binding's parameter renamed or
		// removed, decision 10), drawn as pins of their own marked as errors, each with the reason its tooltip shows.
		std::map<GraphPinRef, std::string> m_StalePins{};
		bool m_LayoutStale = true;
		GraphNodeMetrics m_Metrics{};
		glm::vec2 m_CanvasMinimum = glm::vec2(0.0f); // the canvas's top-left corner on screen, as last drawn
		glm::vec2 m_CanvasSize = glm::vec2(0.0f);
		std::vector<uint32_t> m_Selection{};
		std::map<uint32_t, glm::vec2> m_PendingPositions{}; // moved nodes shown at their new places until the next read

		// The gesture in progress.
		CanvasDrag m_Drag = CanvasDrag::None;
		glm::vec2 m_DragStart = glm::vec2(0.0f);   // graph units
		glm::vec2 m_DragCurrent = glm::vec2(0.0f); // graph units
		std::optional<GraphPinHit> m_LinkFrom{};
		std::optional<GraphLink> m_Detached{}; // a link dragged off its input
		bool m_AddToSelection = false;

		// Menus.
		bool m_OpenNodeMenu = false;
		bool m_OpenContextMenu = false;
		glm::vec2 m_MenuPosition = glm::vec2(0.0f); // graph units: where the menu's node goes
		std::optional<GraphPinHit> m_MenuLink{};    // a dropped link's pin: the menu adds a linked node
		std::optional<GraphLink> m_ContextLink{};   // the link a right-click named
		std::string m_MenuSearch{};
		std::optional<GraphDocument> m_Clipboard{}; // Ctrl+C's nodes, which Ctrl+V pastes

		// The side panel.
		bool m_ShowSidePanel = true;
		std::optional<FieldEdit> m_FieldEdit{};
		std::string m_NewMemberName{};
		int m_NewMemberKind = 0;
		int m_NewMemberElement = 0;
		int m_NewMemberAssetType = 0;
		std::string m_NewMemberChoices{};
		bool m_NewMemberVariable = false;
		std::string m_NewMethodName{}; // M39
	};

}
