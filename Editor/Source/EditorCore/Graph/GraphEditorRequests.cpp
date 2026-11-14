#include "EditorPCH.h"
#include "EditorCore/Graph/GraphEditorRequests.h"

#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <utility>
#include <vector>

// The requests the graph canvas submits (Docs/Decisions/0035-m24-contract.md decision 15, W24.2's stream A).

namespace Engine {

	namespace Utils {

		// The method of every composed action's request.
		static constexpr std::string_view BatchMethod = "edit.batch";

		// `params` written through its registered struct.
		template<typename Params>
		static Result<Json> WriteGraphParams(const TypeRegistry& types, const Params& params)
		{
			const StructInfo* type = types.FindStruct<Params>();
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the graph methods' types are not registered in this type registry");
			return type->ToJson(&params);
		}

		template<typename Params>
		static Result<GraphEditorRequest> MakeGraphRequest(const TypeRegistry& types, GraphEditorAction action, const Params& params)
		{
			ENGINE_TRY_ASSIGN(Json json, WriteGraphParams(types, params));
			return GraphEditorRequest{ .Action = action, .Method = std::string(GetGraphEditorActionMethod(action)), .Params = std::move(json) };
		}

		// One edit.batch op; `name` empty for an unnamed op.
		static Json MakeBatchOp(std::string_view method, std::string_view name, Json params)
		{
			Json op{ { "method", std::string(method) }, { "params", std::move(params) } };
			if (!name.empty())
				op["name"] = std::string(name);
			return op;
		}

		// The "$ref" to the id of the node the op named `name` added.
		static Json NodeReference(std::string_view name)
		{
			return Json{ { "$ref", std::format("{}.node", name) } };
		}

		// The action `action`'s request for `params`, whose list `items` (the nodes to remove, the positions to set) may hold
		// more items than one call of its method takes (MaxGraphMethodItems): one call while it fits, else one edit.batch
		// labelled `label` of calls of at most that many items each, in order, so the canvas's edit of a large selection is
		// still one undo step (M24's review: a graph holds up to MaxGraphNodes nodes, which a box selection may take).
		template<typename Params, typename Item>
		static Result<GraphEditorRequest> MakeSplitGraphRequest(const TypeRegistry& types, GraphEditorAction action, std::string_view label, const Params& params,
			std::vector<Item> Params::* items)
		{
			const std::vector<Item>& all = params.*items;
			if (all.size() <= MaxGraphMethodItems)
				return MakeGraphRequest(types, action, params);
			if ((all.size() + MaxGraphMethodItems - 1) / MaxGraphMethodItems > MaxGraphEditorBatchOps)
				return MakeError(ErrorCode::InvalidArgument, "{} items are more than one edit can take ({} at most)", all.size(), MaxGraphMethodItems * MaxGraphEditorBatchOps);
			const std::string_view method = GetGraphEditorActionMethod(action);
			Json ops = Json::array();
			for (size_t first = 0; first < all.size(); first += MaxGraphMethodItems)
			{
				Params part = params;
				(part.*items).assign(all.begin() + static_cast<std::ptrdiff_t>(first), all.begin() + static_cast<std::ptrdiff_t>(std::min(all.size(), first + MaxGraphMethodItems)));
				ENGINE_TRY_ASSIGN(Json json, WriteGraphParams(types, part));
				ops.push_back(MakeBatchOp(method, "", std::move(json)));
			}
			return GraphEditorRequest{ .Action = action, .Method = std::string(BatchMethod), .Params = Json{ { "label", std::string(label) }, { "ops", std::move(ops) } } };
		}

	}

	Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphCreateParams& params)
	{
		return Utils::MakeGraphRequest(types, GraphEditorAction::CreateGraph, params);
	}

	Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphAddNodeParams& params)
	{
		return Utils::MakeGraphRequest(types, GraphEditorAction::AddNode, params);
	}

	Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphRemoveNodesParams& params)
	{
		return Utils::MakeSplitGraphRequest(types, GraphEditorAction::RemoveNodes, "Remove Graph Nodes", params, &GraphRemoveNodesParams::Nodes);
	}

	Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphMoveNodesParams& params)
	{
		return Utils::MakeSplitGraphRequest(types, GraphEditorAction::MoveNodes, "Move Graph Nodes", params, &GraphMoveNodesParams::Positions);
	}

	Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphConnectParams& params)
	{
		return Utils::MakeGraphRequest(types, GraphEditorAction::Connect, params);
	}

	Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphDisconnectParams& params)
	{
		return Utils::MakeGraphRequest(types, GraphEditorAction::Disconnect, params);
	}

	Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphSetValuesParams& params)
	{
		return Utils::MakeGraphRequest(types, GraphEditorAction::SetValues, params);
	}

	Result<GraphEditorRequest> MakeGraphEditorRequest(const TypeRegistry& types, const GraphSetMembersParams& params)
	{
		return Utils::MakeGraphRequest(types, GraphEditorAction::SetMembers, params);
	}

	GraphEditorRequest MakeGraphUndoRequest()
	{
		return GraphEditorRequest{ .Action = GraphEditorAction::Undo, .Method = std::string(GetGraphEditorActionMethod(GraphEditorAction::Undo)), .Params = Json::object() };
	}

	GraphEditorRequest MakeGraphRedoRequest()
	{
		return GraphEditorRequest{ .Action = GraphEditorAction::Redo, .Method = std::string(GetGraphEditorActionMethod(GraphEditorAction::Redo)), .Params = Json::object() };
	}

	Result<GraphEditorRequest> MakeGraphLinkedNodeRequest(const TypeRegistry& types, const GraphAddNodeParams& node, const GraphPinRef& existing,
		GraphPinDirection direction, std::string_view pin)
	{
		ENGINE_TRY_ASSIGN(Json added, Utils::WriteGraphParams(types, node));
		const GraphPinRef created{ .Node = 0, .Pin = std::string(pin) };
		const bool fromExisting = direction == GraphPinDirection::Output;
		const GraphConnectParams connect{ .Graph = node.Graph, .From = fromExisting ? existing : created, .To = fromExisting ? created : existing };
		ENGINE_TRY_ASSIGN(Json link, Utils::WriteGraphParams(types, connect));
		link[fromExisting ? "to" : "from"]["Node"] = Utils::NodeReference("node");
		Json ops = Json::array();
		ops.push_back(Utils::MakeBatchOp("graph.addNode", "node", std::move(added)));
		ops.push_back(Utils::MakeBatchOp("graph.connect", "", std::move(link)));
		return GraphEditorRequest{ .Action = GraphEditorAction::AddLinkedNode,
			.Method = std::string(GetGraphEditorActionMethod(GraphEditorAction::AddLinkedNode)),
			.Params = Json{ { "label", "Add Linked Graph Node" }, { "ops", std::move(ops) } } };
	}

	Result<GraphEditorRequest> MakeGraphMoveLinkRequest(const TypeRegistry& types, std::string_view graph, const GraphLink& link, const GraphPinRef& to)
	{
		ENGINE_TRY_ASSIGN(Json disconnect, Utils::WriteGraphParams(types, GraphDisconnectParams{ .Graph = std::string(graph), .From = link.From, .To = link.To }));
		ENGINE_TRY_ASSIGN(Json connect, Utils::WriteGraphParams(types, GraphConnectParams{ .Graph = std::string(graph), .From = link.From, .To = to }));
		Json ops = Json::array();
		ops.push_back(Utils::MakeBatchOp("graph.disconnect", "", std::move(disconnect)));
		ops.push_back(Utils::MakeBatchOp("graph.connect", "", std::move(connect)));
		return GraphEditorRequest{ .Action = GraphEditorAction::MoveLink,
			.Method = std::string(GetGraphEditorActionMethod(GraphEditorAction::MoveLink)),
			.Params = Json{ { "label", "Move Graph Link" }, { "ops", std::move(ops) } } };
	}

	GraphDocument CopyGraphNodes(const GraphDocument& graph, std::span<const uint32_t> nodes)
	{
		GraphDocument copied;
		copied.Name = graph.Name;
		for (const GraphNode& node : graph.Nodes)
		{
			if (std::ranges::find(nodes, node.Id) != nodes.end())
				copied.Nodes.push_back(node);
		}
		const auto kept = [&copied](uint32_t id)
		{
			return FindGraphNode(copied, id) != nullptr;
		};
		for (const GraphLink& link : graph.Links)
		{
			if (kept(link.From.Node) && kept(link.To.Node))
				copied.Links.push_back(link);
		}
		return copied;
	}

	Result<GraphEditorRequest> MakeGraphDuplicateRequest(const TypeRegistry& types, std::string_view graph, const GraphDocument& copied,
		const glm::vec2& offset, bool paste)
	{
		if (copied.Nodes.empty())
			return MakeError(ErrorCode::InvalidArgument, "there are no graph nodes to {}", paste ? "paste" : "duplicate");
		if (copied.Nodes.size() + copied.Links.size() > MaxGraphEditorBatchOps)
			return MakeError(ErrorCode::InvalidArgument, "{} graph nodes and {} links are more than one edit can {} ({} at most together)", copied.Nodes.size(),
				copied.Links.size(), paste ? "paste" : "duplicate", MaxGraphEditorBatchOps);
		Json ops = Json::array();
		std::map<uint32_t, std::string> names;
		for (const GraphNode& node : copied.Nodes)
		{
			const std::string name = std::format("n{}", node.Id);
			const GraphAddNodeParams params{ .Graph = std::string(graph),
				.Kind = node.Kind,
				.Position = glm::clamp(node.Position + offset, glm::vec2(-MaxGraphPosition), glm::vec2(MaxGraphPosition)),
				.Values = node.Values };
			ENGINE_TRY_ASSIGN(Json added, Utils::WriteGraphParams(types, params));
			ops.push_back(Utils::MakeBatchOp("graph.addNode", name, std::move(added)));
			names.emplace(node.Id, name);
		}
		for (const GraphLink& link : copied.Links)
		{
			const auto from = names.find(link.From.Node);
			const auto to = names.find(link.To.Node);
			if (from == names.end() || to == names.end())
				continue;
			ENGINE_TRY_ASSIGN(Json connect, Utils::WriteGraphParams(types, GraphConnectParams{ .Graph = std::string(graph), .From = link.From, .To = link.To }));
			connect["from"]["Node"] = Utils::NodeReference(from->second);
			connect["to"]["Node"] = Utils::NodeReference(to->second);
			ops.push_back(Utils::MakeBatchOp("graph.connect", "", std::move(connect)));
		}
		return GraphEditorRequest{ .Action = GraphEditorAction::DuplicateNodes,
			.Method = std::string(GetGraphEditorActionMethod(GraphEditorAction::DuplicateNodes)),
			.Params = Json{ { "label", paste ? "Paste Graph Nodes" : "Duplicate Graph Nodes" }, { "ops", std::move(ops) } } };
	}

}
