#include "EditorPCH.h"
#include "Editor/Panels/GraphEditorPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Graph/GraphNodeMenu.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>

// The graph editor (Docs/Decisions/0035-m24-contract.md decision 15, W24.2's stream A), with stream B's live values (decision
// 14) joined at W24.2's integration.

namespace Engine {

	namespace {

		// The field a value is edited with (a pin's literal, a member's default).
		enum class GraphValueWidget : uint8_t
		{
			None,
			Bool,
			Number,
			Integer,
			String,
			Vector,
			Color,
			Quat,
			Choice,
			Asset,
			Any
		};

		// GraphValueKind's names in value order: a field takes the first 11, a variable every one (M39's Any last), an Array's
		// element the first 10 (GraphDocument.h).
		constexpr std::array<std::string_view, 12> GraphValueKindNames = { "Number", "Integer", "Bool", "String", "Vector", "Color", "Quat",
			"Entity", "Asset", "Enum", "Array", "Any" };
		constexpr int FieldKindCount = 11;
		constexpr int ElementKindCount = 10;

		// The kinds a method's parameter or result may have (M39, IsGraphMethodValueKind), in GraphValueKind's order.
		constexpr std::array<GraphValueKind, 10> MethodValueKinds = { GraphValueKind::Number, GraphValueKind::Integer, GraphValueKind::Bool,
			GraphValueKind::String, GraphValueKind::Vector, GraphValueKind::Color, GraphValueKind::Quat, GraphValueKind::Entity, GraphValueKind::Asset,
			GraphValueKind::Any };

		// A pin the node's kind no longer has, and its label (M24's review): the error outline's colour.
		constexpr ImU32 StalePinColor = IM_COL32(236, 86, 86, 255);

	}

	namespace Utils {

		static ImVec2 ToImVec2(const glm::vec2& value)
		{
			return ImVec2(value.x, value.y);
		}

		static glm::vec2 ToVec2(const ImVec2& value)
		{
			return glm::vec2(value.x, value.y);
		}

		static ImU32 GraphPinColor(GraphPinType type)
		{
			switch (type)
			{
				case GraphPinType::Exec:   return IM_COL32(232, 232, 232, 255);
				case GraphPinType::Any:    return IM_COL32(168, 168, 168, 255);
				case GraphPinType::Bool:   return IM_COL32(222, 92, 92, 255);
				case GraphPinType::Number: return IM_COL32(118, 204, 120, 255);
				case GraphPinType::String: return IM_COL32(230, 122, 214, 255);
				case GraphPinType::Vector: return IM_COL32(238, 200, 84, 255);
				case GraphPinType::Color:  return IM_COL32(104, 172, 250, 255);
				case GraphPinType::Quat:   return IM_COL32(162, 140, 250, 255);
				case GraphPinType::Entity: return IM_COL32(90, 210, 206, 255);
				case GraphPinType::Asset:  return IM_COL32(246, 150, 82, 255);
				case GraphPinType::Enum:   return IM_COL32(204, 200, 120, 255);
				case GraphPinType::Object: return IM_COL32(112, 142, 196, 255);
			}
			return IM_COL32(168, 168, 168, 255);
		}

		static ImU32 GraphCategoryColor(GraphNodeCategory category)
		{
			switch (category)
			{
				case GraphNodeCategory::Event:    return IM_COL32(150, 56, 56, 255);
				case GraphNodeCategory::Flow:     return IM_COL32(86, 88, 100, 255);
				case GraphNodeCategory::Member:   return IM_COL32(52, 104, 140, 255);
				case GraphNodeCategory::Operator: return IM_COL32(64, 112, 74, 255);
				case GraphNodeCategory::Call:     return IM_COL32(56, 84, 142, 255);
				case GraphNodeCategory::Property: return IM_COL32(104, 74, 132, 255);
				case GraphNodeCategory::Constant: return IM_COL32(118, 96, 56, 255);
			}
			return IM_COL32(86, 88, 100, 255);
		}

		// The members a graph's own kinds are listed for: each field's and variable's name and kind, and (M39) each method's
		// name and signature, whose Method and Return kinds' pins follow it.
		static std::string DescribeGraphMembers(const GraphDocument& graph)
		{
			std::string members;
			for (const GraphField& field : graph.Fields)
				members += std::format("{}:{};", field.Name, std::to_underlying(field.Kind));
			for (const GraphVariable& variable : graph.Variables)
				members += std::format("{}:{};", variable.Name, std::to_underlying(variable.Kind));
			for (const GraphMethod& method : graph.Methods)
			{
				members += std::format("{}(", method.Name);
				for (const GraphParameter& parameter : method.Parameters)
					members += std::format("{}:{}:{},", parameter.Name, std::to_underlying(parameter.Kind), parameter.AssetType);
				for (const GraphResult& result : method.Results)
					members += std::format(")->{}:{}", std::to_underlying(result.Kind), result.AssetType);
				members += ";";
			}
			return members;
		}

		// A method's signature as the side panel shows it (M39): "(hit: Any, amount: Number) -> Bool".
		static std::string DescribeMethodSignature(const GraphMethod& method)
		{
			// A kind's name; a value past the enum's (never in a valid graph) as the last one's.
			const auto kindName = [](GraphValueKind kind)
			{
				return GraphValueKindNames[std::min<size_t>(std::to_underlying(kind), GraphValueKindNames.size() - 1)];
			};
			std::string signature = "(";
			for (size_t index = 0; index < method.Parameters.size(); ++index)
			{
				const GraphParameter& parameter = method.Parameters[index];
				signature += std::format("{}{}: {}", index == 0 ? "" : ", ", parameter.Name, kindName(parameter.Kind));
			}
			signature += ")";
			if (!method.Results.empty())
				signature += std::format(" -> {}", kindName(method.Results.front().Kind));
			return signature;
		}

		// The first name "arg<n>" (n from 1) that `method` has no parameter of: a new parameter's.
		static std::string MakeParameterName(const GraphMethod& method)
		{
			for (size_t number = 1;; ++number)
			{
				std::string name = std::format("arg{}", number);
				if (std::ranges::find(method.Parameters, name, &GraphParameter::Name) == method.Parameters.end())
					return name;
			}
		}

		// The JSON number a .graph file holds for `value` (GraphDocument.h's number rule): the float's shortest spelling read
		// back, so a fractional literal is never refused for holding more digits than its float keeps; 0 for -0 and for a
		// non-finite value.
		static Json ToGraphNumber(float value)
		{
			if (value == 0.0f || !std::isfinite(value))
				return Json(0);
			std::array<char, 32> buffer{};
			const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
			if (error != std::errc())
				return Json(static_cast<double>(value));
			Result<Json> parsed = JsonReader::Parse(std::string_view(buffer.data(), static_cast<size_t>(end - buffer.data())));
			return parsed.has_value() ? std::move(*parsed) : Json(static_cast<double>(value));
		}

		static float ReadGraphFloat(const Json& value, float fallback)
		{
			return value.is_number() ? JsonReader(value).ReadFloat().value_or(fallback) : fallback;
		}

		// The components of an array of `count` numbers, `fallback`'s where `value` has none.
		template<size_t Count>
		static std::array<float, Count> ReadGraphFloats(const Json& value, const std::array<float, Count>& fallback)
		{
			std::array<float, Count> components = fallback;
			if (!value.is_array() || value.size() != Count)
				return components;
			for (size_t index = 0; index < Count; ++index)
				components[index] = ReadGraphFloat(value[index], fallback[index]);
			return components;
		}

		template<size_t Count>
		static Json WriteGraphFloats(const std::array<float, Count>& components)
		{
			Json array = Json::array();
			for (const float component : components)
				array.push_back(ToGraphNumber(component));
			return array;
		}

		static GraphValueWidget GetPinWidget(GraphPinType type)
		{
			switch (type)
			{
				case GraphPinType::Bool:   return GraphValueWidget::Bool;
				case GraphPinType::Number: return GraphValueWidget::Number;
				case GraphPinType::String: return GraphValueWidget::String;
				case GraphPinType::Vector: return GraphValueWidget::Vector;
				case GraphPinType::Color:  return GraphValueWidget::Color;
				case GraphPinType::Quat:   return GraphValueWidget::Quat;
				case GraphPinType::Enum:   return GraphValueWidget::Choice;
				case GraphPinType::Asset:  return GraphValueWidget::Asset;
				case GraphPinType::Any:    return GraphValueWidget::Any;
				case GraphPinType::Exec:
				case GraphPinType::Entity:
				case GraphPinType::Object: return GraphValueWidget::None; // linked only
			}
			return GraphValueWidget::None;
		}

		static GraphValueWidget GetMemberWidget(GraphValueKind kind)
		{
			switch (kind)
			{
				case GraphValueKind::Number:  return GraphValueWidget::Number;
				case GraphValueKind::Integer: return GraphValueWidget::Integer;
				case GraphValueKind::Bool:    return GraphValueWidget::Bool;
				case GraphValueKind::String:  return GraphValueWidget::String;
				case GraphValueKind::Vector:  return GraphValueWidget::Vector;
				case GraphValueKind::Color:   return GraphValueWidget::Color;
				case GraphValueKind::Quat:    return GraphValueWidget::Quat;
				case GraphValueKind::Enum:    return GraphValueWidget::Choice;
				case GraphValueKind::Entity:
				case GraphValueKind::Asset:
				case GraphValueKind::Array:
				case GraphValueKind::Any:     return GraphValueWidget::None; // nil and [] by default (GraphDocument.h)
			}
			return GraphValueWidget::None;
		}

		static std::string_view GetGraphValueKindName(GraphValueKind kind)
		{
			const auto index = static_cast<size_t>(std::to_underlying(kind));
			return index < GraphValueKindNames.size() ? GraphValueKindNames[index] : std::string_view("Number");
		}

		// The Default a new member of `kind` takes (GraphDocument.h's spellings).
		static Json MakeMemberDefault(GraphValueKind kind, std::span<const std::string> choices)
		{
			switch (kind)
			{
				case GraphValueKind::Number:
				case GraphValueKind::Integer: return Json(0);
				case GraphValueKind::Bool:    return Json(false);
				case GraphValueKind::String:  return Json("");
				case GraphValueKind::Vector:  return Json::array({ 0, 0, 0 });
				case GraphValueKind::Color:   return Json::array({ 1, 1, 1, 1 });
				case GraphValueKind::Quat:    return Json::array({ 0, 0, 0, 1 });
				case GraphValueKind::Enum:    return choices.empty() ? Json("") : Json(choices.front());
				case GraphValueKind::Array:   return Json::array();
				case GraphValueKind::Entity:
				case GraphValueKind::Asset:
				case GraphValueKind::Any:     return Json();
			}
			return Json();
		}

		// The AssetType names other than None, in the enumeration's order.
		static std::vector<std::string_view> GetGraphAssetTypeNames()
		{
			std::vector<std::string_view> names;
			for (uint16_t value = 1;; ++value)
			{
				const std::string_view name = AssetTypeToString(static_cast<AssetType>(value));
				if (name == "None")
					break;
				names.push_back(name);
			}
			return names;
		}

		// "A, B ,C" as its non-empty trimmed parts.
		static std::vector<std::string> SplitGraphChoices(std::string_view text)
		{
			std::vector<std::string> choices;
			while (!text.empty())
			{
				const size_t comma = text.find(',');
				std::string_view part = text.substr(0, comma);
				text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
				while (!part.empty() && part.front() == ' ')
					part.remove_prefix(1);
				while (!part.empty() && part.back() == ' ')
					part.remove_suffix(1);
				if (!part.empty())
					choices.emplace_back(part);
			}
			return choices;
		}

		// The value an Any literal's text gives: JSON null, a boolean, a number (as a .graph file holds it) or, for any other
		// text, the text as a string.
		static Json ParseAnyLiteral(const std::string& text)
		{
			Result<Json> parsed = JsonReader::Parse(text);
			if (!parsed.has_value() || !(parsed->is_null() || parsed->is_boolean() || parsed->is_number() || parsed->is_string()))
				return Json(text);
			if (parsed->is_number_float())
				return ToGraphNumber(ReadGraphFloat(*parsed, 0.0f));
			return std::move(*parsed);
		}

		// Draws `text` at `position` with the canvas's zoomed font size, clipped to `clipRight`; nothing when too small to read.
		static void DrawCanvasText(ImDrawList& draw, const glm::vec2& position, float size, ImU32 color, std::string_view text, float clipRight)
		{
			if (size < 6.0f || text.empty())
				return;
			const ImVec4 clip(position.x, position.y, clipRight, position.y + size * 2.0f);
			draw.AddText(ImGui::GetFont(), size, ToImVec2(position), color, text.data(), text.data() + text.size(), 0.0f, &clip);
		}

		static float MeasureCanvasText(float size, std::string_view text)
		{
			return ImGui::GetFont()->CalcTextSizeA(size, std::numeric_limits<float>::max(), 0.0f, text.data(), text.data() + text.size()).x;
		}

	}

	void GraphEditorPanel::Open(const GraphEditorTarget& target)
	{
		if (target.Graph != m_Graph)
		{
			Reset();
			m_Graph = target.Graph;
			m_Reference = target.Graph.ToString();
		}
		else if (m_ReadTicket != 0)
		{
			// The graph is open and a read of it is in flight, asked before this target (without the text and line map a line
			// target needs): its result is dropped, and the read the next frame submits is the target's.
			m_AbandonedReads.push_back(std::exchange(m_ReadTicket, 0));
		}
		m_Target = target.Graph.IsValid() ? std::optional<GraphEditorTarget>(target) : std::nullopt;
		// The read held is older than the target, so the target resolves against the next one (ApplyRead).
		m_TargetRead = false;
		m_ReadStale = true;
		m_FocusWindow = target.Graph.IsValid();
		if (!target.Graph.IsValid())
			Close();
	}

	void GraphEditorPanel::Close()
	{
		Reset();
		m_Graph = {};
		m_Reference.clear();
	}

	void GraphEditorPanel::Reset()
	{
		// A pending read is of the graph that leaves, so the next frame discards it; a pending edit still runs, and its outcome is
		// still taken (PollEdits), so a refusal is reported even after the window closed.
		for (const uint64_t ticket : { m_ReadTicket, m_NodesTicket })
		{
			if (ticket != 0)
				m_AbandonedReads.push_back(ticket);
		}
		m_Target.reset();
		m_TargetRead = false;
		m_Read.reset();
		m_ReadError.clear();
		m_ReadTicket = 0;
		m_ReadStale = true;
		m_Library.clear();
		m_LibraryMembers.clear();
		m_NodesTicket = 0;
		m_NodesStale = true;
		m_Error.clear();
		m_HasSignals = false;
		m_View = {};
		m_ViewFramed = false;
		m_Shapes.clear();
		m_Layouts.clear();
		m_NodeFindings.clear();
		m_LinkedPins.clear();
		m_StalePins.clear();
		m_LayoutStale = true;
		m_Selection.clear();
		m_PendingPositions.clear();
		m_Drag = CanvasDrag::None;
		m_LinkFrom.reset();
		m_Detached.reset();
		m_OpenNodeMenu = false;
		m_OpenContextMenu = false;
		m_MenuLink.reset();
		m_ContextLink.reset();
		m_MenuSearch.clear();
		m_FieldEdit.reset();
	}

	std::optional<glm::vec2> GraphEditorPanel::GetPinScreenPosition(const GraphPinRef& pin) const
	{
		const GraphPinLayout* layout = FindGraphPinLayout(m_Layouts, pin);
		if (layout == nullptr || !m_Graph.IsValid())
			return std::nullopt;
		return m_CanvasMinimum + GraphToCanvas(m_View, layout->Center);
	}

	std::optional<std::pair<glm::vec2, glm::vec2>> GraphEditorPanel::GetNodeScreenRect(uint32_t node) const
	{
		const auto found = std::ranges::find(m_Layouts, node, &GraphNodeLayout::Node);
		if (found == m_Layouts.end() || !m_Graph.IsValid())
			return std::nullopt;
		return std::pair(m_CanvasMinimum + GraphToCanvas(m_View, found->Minimum), m_CanvasMinimum + GraphToCanvas(m_View, found->Maximum));
	}

	const GraphLiveValuesResult* GraphEditorPanel::FindShownLiveValues() const
	{
		const std::optional<GraphLiveValuesResult>& live = m_Live.GetValues();
		return live.has_value() && m_Graph.IsValid() && live->Graph.Id == m_Reference ? &*live : nullptr;
	}

	std::vector<std::string> GraphEditorPanel::GetNodeLiveLines(uint32_t node) const
	{
		return FindShownLiveValues() != nullptr ? m_Live.GetNodeLines(node) : std::vector<std::string>{};
	}

	GraphEditorPanel::NodeOutline GraphEditorPanel::GetNodeOutline(uint32_t node) const
	{
		if (std::ranges::binary_search(m_Selection, node))
			return NodeOutline::Selected;
		if (FindShownLiveValues() != nullptr && !m_Live.GetNodeErrors(node).empty())
			return NodeOutline::Error;
		const auto finding = m_NodeFindings.find(node);
		if (finding == m_NodeFindings.end())
			return NodeOutline::None;
		return finding->second >= 2 ? NodeOutline::Error : NodeOutline::Warning;
	}

	void GraphEditorPanel::Submit(EditorPanelContext& context, Result<GraphEditorRequest> request)
	{
		if (!request)
		{
			m_Error = request.error().GetMessageText();
			ENGINE_ERROR("Graph editor: {}", request.error().ToString());
			return;
		}
		auto ticket = context.Actions.Submit(request->Method, request->Params);
		if (!ticket)
		{
			m_Error = ticket.error().GetMessageText();
			ENGINE_ERROR("Graph editor: {}", ticket.error().ToString());
			return;
		}
		m_EditTickets.push_back(*ticket);
		m_Error.clear();
	}

	void GraphEditorPanel::SubmitRead(EditorPanelContext& context)
	{
		// The text and its line map only when a target needs them to find its node: a graph's text may be large.
		const bool luau = m_Target.has_value() && m_Target->Line != 0;
		auto ticket = context.Actions.Submit("graph.read", Json{ { "graph", m_Reference }, { "luau", luau } });
		if (!ticket)
		{
			m_ReadError = ticket.error().GetMessageText();
			return;
		}
		m_ReadTicket = *ticket;
		m_ReadStale = false;
	}

	void GraphEditorPanel::SubmitNodes(EditorPanelContext& context)
	{
		auto ticket = context.Actions.Submit("graph.nodes", Json{ { "graph", m_Reference } });
		if (!ticket)
		{
			m_ReadError = ticket.error().GetMessageText();
			return;
		}
		m_NodesTicket = *ticket;
		m_NodesStale = false;
		if (m_Read.has_value())
			m_LibraryMembers = Utils::DescribeGraphMembers(m_Read->Document);
	}

	void GraphEditorPanel::ApplyRead(GraphReadResult read)
	{
		m_Read = std::move(read);
		m_TargetRead = true; // submitted after any target Open set, since Open drops an older read in flight
		m_ReadError.clear();
		m_PendingPositions.clear();
		// The field being edited keeps its candidate (its text, or its drag) across the read; the read applies to the rest.
		if (m_FieldEdit.has_value() && (m_FieldEdit->Submitted || !m_FieldEdit->Active))
			m_FieldEdit.reset();
		std::erase_if(m_Selection, [this](uint32_t id)
		{
			return FindGraphNode(m_Read->Document, id) == nullptr;
		});
		m_NodeFindings.clear();
		for (const GraphDiagnostic& diagnostic : m_Read->Diagnostics)
		{
			if (diagnostic.Node == 0)
				continue;
			const size_t level = diagnostic.Severity == DiagnosticSeverity::Error && !diagnostic.Incomplete ? 2 : 1;
			size_t& worst = m_NodeFindings[diagnostic.Node];
			worst = std::max(worst, level);
		}
		m_LinkedPins.clear();
		for (const GraphLink& link : m_Read->Document.Links)
		{
			m_LinkedPins.insert(link.From);
			m_LinkedPins.insert(link.To);
		}
		// The library's Self kinds follow the members.
		if (Utils::DescribeGraphMembers(m_Read->Document) != m_LibraryMembers)
			m_NodesStale = true;
		m_LayoutStale = true;
	}

	void GraphEditorPanel::PollReads(EditorPanelContext& context)
	{
		const TypeRegistry& types = context.Editor.GetTypeRegistry();
		if (m_ReadTicket != 0)
		{
			auto result = context.Actions.TakeResult(m_ReadTicket);
			if (!result || result->has_value())
			{
				m_ReadTicket = 0;
				if (!result)
					m_ReadError = result.error().GetMessageText();
				else if (!**result)
					m_ReadError = (**result).error().GetMessageText();
				else if (const StructInfo* type = types.FindStruct<GraphReadResult>(); type == nullptr)
					m_ReadError = "the graph methods' types are not registered";
				else
				{
					GraphReadResult read;
					if (const Status decoded = type->FromJson(&read, JsonReader(***result), {}); !decoded)
						m_ReadError = decoded.error().GetMessageText();
					else
						ApplyRead(std::move(read));
				}
			}
		}
		if (m_NodesTicket != 0)
		{
			auto result = context.Actions.TakeResult(m_NodesTicket);
			if (!result || result->has_value())
			{
				m_NodesTicket = 0;
				if (!result)
					m_ReadError = result.error().GetMessageText();
				else if (!**result)
					m_ReadError = (**result).error().GetMessageText();
				else if (const StructInfo* type = types.FindStruct<GraphNodesResult>(); type == nullptr)
					m_ReadError = "the graph methods' types are not registered";
				else
				{
					GraphNodesResult nodes;
					if (const Status decoded = type->FromJson(&nodes, JsonReader(***result), {}); !decoded)
						m_ReadError = decoded.error().GetMessageText();
					else
					{
						m_Library = std::move(nodes.Nodes);
						m_LayoutStale = true;
					}
				}
			}
		}
	}

	void GraphEditorPanel::PollEdits(EditorPanelContext& context)
	{
		for (const uint64_t ticket : std::exchange(m_AbandonedReads, {}))
			static_cast<void>(context.Actions.Discard(ticket)); // NotFound once its result was taken: nothing is left to release
		std::erase_if(m_EditTickets, [this, &context](uint64_t ticket)
		{
			auto result = context.Actions.TakeResult(ticket);
			if (result && !result->has_value())
				return false;
			const auto refused = [this](const Error& error)
			{
				m_Error = error.GetMessageText();
				if (!error.GetHint().empty())
					m_Error += std::format(" ({})", error.GetHint());
				ENGINE_ERROR("Graph editor: {}", error.ToString());
				m_FieldEdit.reset(); // the field shows the stored value again
				m_PendingPositions.clear();
				m_LayoutStale = true;
			};
			if (!result)
				refused(result.error());
			else if (!**result)
				refused((**result).error());
			m_ReadStale = true;
			return true;
		});
	}

	void GraphEditorPanel::DetectChanges(EditorContext& editor)
	{
		const CommandHistory& history = editor.GetHistory();
		const EditorAssetManager& assets = editor.GetAssets();
		const uint64_t sequence = history.GetCurrentSequence();
		const size_t undo = history.GetUndoCount();
		const size_t redo = history.GetRedoCount();
		const uint64_t version = assets.GetVersion(m_Graph);
		const uint64_t registry = assets.GetRegistry().GetRevision();
		if (!m_HasSignals || sequence != m_HistorySequence || undo != m_HistoryUndoCount || redo != m_HistoryRedoCount || version != m_AssetVersion
			|| registry != m_RegistryRevision)
			m_ReadStale = true;
		m_HistorySequence = sequence;
		m_HistoryUndoCount = undo;
		m_HistoryRedoCount = redo;
		m_AssetVersion = version;
		m_RegistryRevision = registry;
		m_HasSignals = true;
	}

	const GraphNodeDescription* GraphEditorPanel::FindDescription(std::string_view kind) const
	{
		const auto found = std::ranges::lower_bound(m_Library, kind, {}, [](const GraphNodeDescription& node)
		{
			return std::string_view(node.Kind);
		});
		return found != m_Library.end() && found->Kind == kind ? &*found : nullptr;
	}

	const GraphPinDescription* GraphEditorPanel::FindPinDescription(const GraphPinRef& pin) const
	{
		if (!m_Read.has_value())
			return nullptr;
		const GraphNode* node = FindGraphNode(m_Read->Document, pin.Node);
		const GraphNodeDescription* description = node != nullptr ? FindDescription(node->Kind) : nullptr;
		if (description == nullptr)
			return nullptr;
		const auto found = std::ranges::find(description->Pins, pin.Pin, &GraphPinDescription::Name);
		return found != description->Pins.end() ? &*found : nullptr;
	}

	void GraphEditorPanel::Select(std::vector<uint32_t> nodes)
	{
		std::ranges::sort(nodes);
		nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
		if (nodes != m_Selection)
		{
			m_Selection = std::move(nodes);
			m_LayoutStale = true; // the selected nodes draw on top
			m_FieldEdit.reset();
		}
	}

	void GraphEditorPanel::Frame(std::span<const uint32_t> nodes, const glm::vec2& canvasSize)
	{
		glm::vec2 minimum(std::numeric_limits<float>::max());
		glm::vec2 maximum(std::numeric_limits<float>::lowest());
		bool any = false;
		for (const GraphNodeLayout& layout : m_Layouts)
		{
			if (!nodes.empty() && std::ranges::find(nodes, layout.Node) == nodes.end())
				continue;
			minimum = glm::min(minimum, layout.Minimum);
			maximum = glm::max(maximum, layout.Maximum);
			any = true;
		}
		if (!any)
		{
			m_View = GraphCanvasView{ .Origin = -canvasSize * 0.5f, .Zoom = 1.0f };
			return;
		}
		m_View = FrameGraphCanvas(minimum, maximum, canvasSize, 48.0f);
	}

	void GraphEditorPanel::RebuildLayouts()
	{
		m_Shapes.clear();
		m_Layouts.clear();
		m_StalePins.clear();
		m_LayoutStale = false;
		if (!m_Read.has_value())
			return;
		const float fontSize = ImGui::GetFontSize();
		m_Metrics = GraphNodeMetrics{ .TitleHeight = fontSize + 10.0f,
			.RowHeight = fontSize + 8.0f,
			.PinRadius = std::max(4.0f, fontSize * 0.3f),
			.Padding = 10.0f,
			.LabelGap = 6.0f,
			.ColumnGap = 20.0f,
			.MinWidth = fontSize * 8.0f };
		const auto measure = [fontSize](std::string_view text)
		{
			return Utils::MeasureCanvasText(fontSize, text);
		};
		// By id, the selected nodes last, so they draw on top and win the hit tests.
		std::vector<const GraphNode*> order;
		for (const GraphNode& node : m_Read->Document.Nodes)
			order.push_back(&node);
		std::stable_partition(order.begin(), order.end(), [this](const GraphNode* node)
		{
			return !std::ranges::binary_search(m_Selection, node->Id);
		});
		const std::vector<GraphLink>& links = m_Read->Document.Links;
		for (const GraphNode* node : order)
		{
			const auto pending = m_PendingPositions.find(node->Id);
			GraphNodeShape shape{ .Node = node->Id, .Position = pending != m_PendingPositions.end() ? pending->second : node->Position, .Title = {}, .Pins = {} };
			if (const GraphNodeDescription* description = FindDescription(node->Kind))
			{
				shape.Title = description->Title.empty() ? node->Kind : description->Title;
				for (const GraphPinDescription& pin : description->Pins)
				{
					std::string label = pin.Name;
					const auto literal = node->Values.find(pin.Name);
					if (pin.Direction == GraphPinDirection::Input && literal != node->Values.end() && !literal->second.Get().is_null()
						&& !m_LinkedPins.contains(GraphPinRef{ .Node = node->Id, .Pin = pin.Name }))
						label += " = " + FormatGraphLiteral(literal->second.Get());
					shape.Pins.push_back({ .Name = pin.Name, .Direction = pin.Direction, .Type = pin.Type, .Label = std::move(label) });
				}
				// M24's review: a link or literal on a pin the kind no longer has (a binding's parameter renamed or removed,
				// decision 10) is a pin of its own, untyped and marked as an error, so it shows and can be removed: its link with
				// Alt+click, the context menu or a drag off it (graph.disconnect), its literal with the side panel's Clear literal
				// (graph.setValues with null). A name the kind uses for another pin is left to the side panel.
				const auto addStale = [this, &shape, description, node](const std::string& name, GraphPinDirection direction, std::string label, std::string reason)
				{
					if (std::ranges::any_of(description->Pins, [&name](const GraphPinDescription& pin)
					{
						return pin.Name == name;
					}))
						return;
					const GraphPinRef ref{ .Node = node->Id, .Pin = name };
					if (m_StalePins.contains(ref))
						return;
					shape.Pins.push_back({ .Name = name, .Direction = direction, .Type = GraphPinType::Any, .Label = std::move(label) });
					m_StalePins.emplace(ref, std::move(reason));
				};
				const std::string what = std::format("node {} ({})", node->Id, node->Kind);
				for (const GraphLink& link : links)
				{
					if (link.To.Node == node->Id)
					{
						addStale(link.To.Pin, GraphPinDirection::Input, link.To.Pin,
							std::format("{} has no input '{}' now, so this link is an error: Alt+click it, or drag it off the pin, to remove it.", what, link.To.Pin));
					}
					if (link.From.Node == node->Id)
					{
						addStale(link.From.Pin, GraphPinDirection::Output, link.From.Pin,
							std::format("{} has no output '{}' now, so this link is an error: Alt+click it to remove it.", what, link.From.Pin));
					}
				}
				for (const auto& [pin, value] : node->Values)
				{
					addStale(pin, GraphPinDirection::Input, std::format("{} = {}", pin, FormatGraphLiteral(value.Get())),
						std::format("{} has no input '{}' now, so its literal is an error: Clear literal in the side panel removes it.", what, pin));
				}
			}
			else
			{
				// A kind the library does not know (a binding removed or renamed, decision 10): its linked pins, untyped.
				shape.Title = node->Kind;
				const auto addPin = [&shape](const std::string& name, GraphPinDirection direction)
				{
					const bool known = std::ranges::any_of(shape.Pins, [&name, direction](const GraphPinShape& pin)
					{
						return pin.Name == name && pin.Direction == direction;
					});
					if (!known)
						shape.Pins.push_back({ .Name = name, .Direction = direction, .Type = GraphPinType::Any, .Label = name });
				};
				for (const GraphLink& link : links)
				{
					if (link.To.Node == node->Id)
						addPin(link.To.Pin, GraphPinDirection::Input);
					if (link.From.Node == node->Id)
						addPin(link.From.Pin, GraphPinDirection::Output);
				}
			}
			m_Layouts.push_back(LayOutGraphNode(shape, m_Metrics, measure));
			m_Shapes.push_back(std::move(shape));
		}
	}

	void GraphEditorPanel::AddNode(EditorPanelContext& context, const GraphNodeDescription& node, const std::string& pin)
	{
		const TypeRegistry& types = context.Editor.GetTypeRegistry();
		const GraphAddNodeParams params{ .Graph = m_Reference,
			.Kind = node.Kind,
			.Position = glm::clamp(glm::round(m_MenuPosition), glm::vec2(-MaxGraphPosition), glm::vec2(MaxGraphPosition)),
			.Values = {} };
		if (m_MenuLink.has_value() && !pin.empty())
			Submit(context, MakeGraphLinkedNodeRequest(types, params, GraphPinRef{ .Node = m_MenuLink->Node, .Pin = m_MenuLink->Pin }, m_MenuLink->Direction, pin));
		else
			Submit(context, MakeGraphEditorRequest(types, params));
	}

	void GraphEditorPanel::RemoveSelection(EditorPanelContext& context)
	{
		if (m_Selection.empty())
			return;
		Submit(context, MakeGraphEditorRequest(context.Editor.GetTypeRegistry(), GraphRemoveNodesParams{ .Graph = m_Reference, .Nodes = m_Selection }));
	}

	void GraphEditorPanel::DuplicateNodes(EditorPanelContext& context, const GraphDocument& copied, const glm::vec2& offset, bool paste)
	{
		if (copied.Nodes.empty())
			return;
		Submit(context, MakeGraphDuplicateRequest(context.Editor.GetTypeRegistry(), m_Reference, copied, glm::round(offset), paste));
	}

	void GraphEditorPanel::SetLiteral(EditorPanelContext& context, uint32_t node, const std::string& pin, const Json& value)
	{
		const GraphSetValuesParams params{ .Graph = m_Reference, .Values = { GraphValueEdit{ .Node = node, .Pin = pin, .Value = VariantValue(value) } } };
		Submit(context, MakeGraphEditorRequest(context.Editor.GetTypeRegistry(), params));
	}

	Status GraphEditorPanel::Draw(EditorPanelContext& context)
	{
		EditorContext& editor = context.Editor;
		const std::filesystem::path project = editor.HasProject() ? editor.GetProject().GetProjectFile() : std::filesystem::path{};
		if (project != m_Project)
		{
			// The graph belonged to the project that closed, unless it was opened since the last frame (m_FocusWindow, which the
			// window takes when it is drawn): a graph of the project now open.
			m_Project = project;
			if (!m_FocusWindow)
				Close();
		}
		PollEdits(context);
		if (m_LiveGraph != m_Graph)
		{
			// Closed, or another graph opened: the live values of the last one stop.
			m_Live.Reset(context.Actions);
			m_LiveGraph = m_Graph;
		}
		if (!m_Graph.IsValid())
			return {};
		PollReads(context);
		if (!editor.GetAssets().GetRegistry().Find(m_Graph))
		{
			// Deleted (moved to the trash) or never a graph of this project: nothing is left to show.
			Close();
			return {};
		}
		DetectChanges(editor);
		if (m_ReadStale && m_ReadTicket == 0)
			SubmitRead(context);
		if (m_NodesStale && m_NodesTicket == 0 && m_Read.has_value())
			SubmitNodes(context);
		// Decision 14: during play, the instance's live values, read between frames (nothing without a play session that runs
		// behaviours, or before the graph's first read names its behaviour).
		m_Live.Update(editor, context.Actions, m_Read.has_value() ? FindGraphLiveTarget(editor, m_Graph, m_Read->Document.Name) : std::nullopt);

		const float scale = Utils::EditorUiScale();
		ImGui::SetNextWindowSize(ImVec2(960.0f * scale, 600.0f * scale), ImGuiCond_FirstUseEver);
		if (std::exchange(m_FocusWindow, false))
			ImGui::SetNextWindowFocus();
		bool open = true;
		if (ImGui::Begin("Graph Editor", &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			DrawToolbar(context);
			if (!m_ReadError.empty())
			{
				ImGui::TextColored(ImVec4(0.95f, 0.48f, 0.48f, 1.0f), "%s", m_ReadError.c_str());
				// A source that does not read is fixed in its text (its GRAPH_INVALID finding says where).
				if (context.OpenSource && ImGui::SmallButton("Open source"))
				{
					const AssetRecord* record = editor.GetAssets().GetRegistry().Find(m_Graph);
					if (record != nullptr)
					{
						const Status opened = context.OpenSource(editor.GetProject().GetRoot() / FileSystem::PathFromUtf8(record->SourcePath.GetPath()), 0);
						if (!opened)
							m_Error = opened.error().GetMessageText();
					}
				}
			}
			if (!m_Error.empty() && Utils::EditorErrorNotice(m_Error))
				m_Error.clear();
			const float sideWidth = m_ShowSidePanel ? 300.0f * scale : 0.0f;
			const float canvasWidth = std::max(ImGui::GetContentRegionAvail().x - sideWidth - (m_ShowSidePanel ? ImGui::GetStyle().ItemSpacing.x : 0.0f), 64.0f);
			if (ImGui::BeginChild("##GraphCanvas", ImVec2(canvasWidth, 0.0f), ImGuiChildFlags_Borders,
					ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove))
				DrawCanvas(context);
			ImGui::EndChild();
			if (m_ShowSidePanel)
			{
				ImGui::SameLine();
				if (ImGui::BeginChild("##GraphSide", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
					DrawSidePanel(context);
				ImGui::EndChild();
			}
			HandleShortcuts(context);
			DrawNodeMenu(context);
			DrawContextMenu(context);
		}
		ImGui::End();
		if (!open)
			Close();
		return {};
	}

	void GraphEditorPanel::DrawToolbar(EditorPanelContext& context)
	{
		EditorContext& editor = context.Editor;
		const bool editable = !editor.IsReadOnly() && !editor.GetPlay().IsTestRunActive();
		const std::string path = editor.GetAssets().GetReferencePath(m_Graph);
		const std::string title = m_Read.has_value() && !m_Read->Document.Name.empty() ? std::format("{}  ({})", m_Read->Document.Name, path) : path;
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(title.c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(!editable || !m_Read.has_value() || m_Library.empty());
		if (Utils::EditorToolbarButton("Add node", "Choose a node to add at the centre of the canvas (or right-click the canvas)."))
		{
			m_MenuPosition = CanvasToGraph(m_View, m_CanvasSize * 0.5f);
			m_MenuLink.reset();
			m_OpenNodeMenu = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!editable || !editor.GetHistory().CanUndo());
		if (Utils::EditorToolbarButton("Undo", "Undo the last change (Ctrl+Z)."))
			Submit(context, MakeGraphUndoRequest());
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!editable || !editor.GetHistory().CanRedo());
		if (Utils::EditorToolbarButton("Redo", "Redo the change undone last (Ctrl+Y)."))
			Submit(context, MakeGraphRedoRequest());
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (Utils::EditorToolbarButton("Frame", "Show the selected nodes, or the whole graph (F)."))
			Frame(m_Selection, m_CanvasSize);
		ImGui::SameLine();
		if (Utils::EditorToolbarButton("Members", "Show or hide the behaviour's members and the selected node's inputs.", m_ShowSidePanel))
			m_ShowSidePanel = !m_ShowSidePanel;
		if (m_Read.has_value())
		{
			size_t errors = 0;
			size_t unfinished = 0;
			size_t warnings = 0;
			for (const GraphDiagnostic& diagnostic : m_Read->Diagnostics)
			{
				if (diagnostic.Incomplete)
					++unfinished;
				else if (diagnostic.Severity == DiagnosticSeverity::Error)
					++errors;
				else
					++warnings;
			}
			ImGui::SameLine();
			if (errors + unfinished + warnings == 0)
				ImGui::TextDisabled("No issues");
			else
				ImGui::TextDisabled("%zu errors, %zu unfinished inputs, %zu warnings", errors, unfinished, warnings);
		}
		else if (m_ReadError.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("Reading the graph...");
		}
		if (const GraphLiveValuesResult* live = FindShownLiveValues())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("Live values of %s at tick %llu", live->Entity.Name.c_str(), static_cast<unsigned long long>(live->Tick));
		}
		else if (const std::optional<GraphLiveValuesResult>& other = m_Live.GetValues(); other.has_value())
		{
			// graph.liveValues names an entity's slot by its behaviour name and reads the first slot of that name (decision 14):
			// an earlier slot holds another graph of this graph's name, so the values are not this graph's and none is shown.
			const std::string behaviour = m_Live.GetTarget().has_value() ? m_Live.GetTarget()->Script : std::string();
			ImGui::TextWrapped("Live values: this graph's slot of %s cannot be read. graph.liveValues reads the first slot whose behaviour is named %s, "
							   "slot %u, which holds another graph (%s).",
				other->Entity.Name.c_str(), behaviour.c_str(), other->Slot, other->Graph.Path.c_str());
		}
		else if (!m_Live.GetError().empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("Live values: %s", m_Live.GetError().c_str());
		}
	}

	void GraphEditorPanel::DrawCanvas(EditorPanelContext& context)
	{
		const glm::vec2 minimum = Utils::ToVec2(ImGui::GetCursorScreenPos());
		const glm::vec2 size = glm::max(Utils::ToVec2(ImGui::GetContentRegionAvail()), glm::vec2(32.0f));
		m_CanvasMinimum = minimum;
		m_CanvasSize = size;
		ImGui::InvisibleButton("##GraphCanvasInput", Utils::ToImVec2(size), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
		const bool hovered = ImGui::IsItemHovered();
		if (m_LayoutStale)
			RebuildLayouts();
		if (m_Read.has_value() && !m_Library.empty())
		{
			if (m_Target.has_value() && m_TargetRead)
			{
				// The node a finding or a caller named (decision 15): selected and framed, resolved against a read asked since
				// the target was set, which carries the text's line map a line target needs.
				const uint32_t node = ResolveGraphEditorTarget(*m_Target, *m_Read);
				if (node != 0)
				{
					Select({ node });
					RebuildLayouts();
					const uint32_t framed[] = { node };
					Frame(framed, size);
					m_ViewFramed = true;
				}
				m_Target.reset();
			}
			if (!m_ViewFramed)
			{
				Frame({}, size);
				m_ViewFramed = true;
			}
		}

		ImDrawList& draw = *ImGui::GetWindowDrawList();
		const glm::vec2 maximum = minimum + size;
		draw.PushClipRect(Utils::ToImVec2(minimum), Utils::ToImVec2(maximum), true);
		draw.AddRectFilled(Utils::ToImVec2(minimum), Utils::ToImVec2(maximum), IM_COL32(30, 31, 34, 255));
		const float zoom = m_View.Zoom;
		const auto toScreen = [this, &minimum](const glm::vec2& point)
		{
			return minimum + GraphToCanvas(m_View, point);
		};

		// The grid: a line every 32 graph units, every fourth one brighter; the minor lines go when they would crowd.
		constexpr float GridStep = 32.0f;
		const glm::vec2 first = glm::floor(m_View.Origin / GridStep) * GridStep;
		const glm::vec2 last = CanvasToGraph(m_View, size);
		if (GridStep * zoom >= 6.0f)
		{
			for (float x = first.x; x <= last.x; x += GridStep)
			{
				const bool major = std::fmod(std::abs(x), GridStep * 4.0f) < 0.5f;
				const float screen = toScreen(glm::vec2(x, 0.0f)).x;
				draw.AddLine(ImVec2(screen, minimum.y), ImVec2(screen, maximum.y), major ? IM_COL32(58, 60, 66, 255) : IM_COL32(42, 43, 48, 255));
			}
			for (float y = first.y; y <= last.y; y += GridStep)
			{
				const bool major = std::fmod(std::abs(y), GridStep * 4.0f) < 0.5f;
				const float screen = toScreen(glm::vec2(0.0f, y)).y;
				draw.AddLine(ImVec2(minimum.x, screen), ImVec2(maximum.x, screen), major ? IM_COL32(58, 60, 66, 255) : IM_COL32(42, 43, 48, 255));
			}
		}

		if (!m_Read.has_value())
		{
			draw.PopClipRect();
			HandleCanvasInput(context, minimum, hovered);
			return;
		}

		// The selected nodes move with a drag until its release submits graph.moveNodes.
		const glm::vec2 moving = m_Drag == CanvasDrag::Move ? m_DragCurrent - m_DragStart : glm::vec2(0.0f);
		const auto offsetOf = [this, &moving](uint32_t node)
		{
			return std::ranges::binary_search(m_Selection, node) ? moving : glm::vec2(0.0f);
		};
		const float thickness = std::max(1.5f, 2.5f * zoom);

		// Links under the nodes, coloured by the value they carry.
		for (const GraphLink& link : m_Read->Document.Links)
		{
			if (m_Detached.has_value() && *m_Detached == link)
				continue;
			const GraphPinLayout* from = FindGraphPinLayout(m_Layouts, link.From);
			const GraphPinLayout* to = FindGraphPinLayout(m_Layouts, link.To);
			if (from == nullptr || to == nullptr)
				continue;
			const auto curve = GetGraphLinkCurve(toScreen(from->Center + offsetOf(link.From.Node)), toScreen(to->Center + offsetOf(link.To.Node)));
			draw.AddBezierCubic(Utils::ToImVec2(curve[0]), Utils::ToImVec2(curve[1]), Utils::ToImVec2(curve[2]), Utils::ToImVec2(curve[3]),
				Utils::GraphPinColor(from->Type), thickness);
		}

		// The nodes: a box, a title bar coloured by category, the pins and their labels, the outline marking the selection and
		// the findings.
		const float fontSize = ImGui::GetFontSize() * zoom;
		const float rounding = 6.0f * zoom;
		for (size_t index = 0; index < m_Layouts.size(); ++index)
		{
			const GraphNodeLayout& layout = m_Layouts[index];
			const GraphNodeShape& shape = m_Shapes[index];
			const glm::vec2 offset = offsetOf(layout.Node);
			const glm::vec2 boxMinimum = toScreen(layout.Minimum + offset);
			const glm::vec2 boxMaximum = toScreen(layout.Maximum + offset);
			if (boxMaximum.x < minimum.x || boxMaximum.y < minimum.y || boxMinimum.x > maximum.x || boxMinimum.y > maximum.y)
				continue;
			const GraphNode* node = FindGraphNode(m_Read->Document, layout.Node);
			const GraphNodeDescription* description = node != nullptr ? FindDescription(node->Kind) : nullptr;
			const ImU32 titleColor = description != nullptr ? Utils::GraphCategoryColor(description->Category) : IM_COL32(120, 40, 40, 255);
			const float titleBottom = boxMinimum.y + m_Metrics.TitleHeight * zoom;
			draw.AddRectFilled(Utils::ToImVec2(boxMinimum), Utils::ToImVec2(boxMaximum), IM_COL32(48, 50, 56, 240), rounding);
			draw.AddRectFilled(Utils::ToImVec2(boxMinimum), ImVec2(boxMaximum.x, titleBottom), titleColor, rounding, ImDrawFlags_RoundCornersTop);
			const glm::vec2 titlePosition = boxMinimum + glm::vec2(m_Metrics.Padding, (m_Metrics.TitleHeight - ImGui::GetFontSize()) * 0.5f) * zoom;
			Utils::DrawCanvasText(draw, titlePosition, fontSize, IM_COL32(240, 240, 240, 255), shape.Title, boxMaximum.x - m_Metrics.Padding * zoom * 0.5f);

			// Decision 14: in play, the node's live lines, its outputs' values first, then the errors its code raised.
			const std::vector<std::string> liveLines = GetNodeLiveLines(layout.Node);
			const size_t liveValueCount = liveLines.empty() ? 0 : m_Live.GetNodeValues(layout.Node).size();

			const NodeOutline mark = GetNodeOutline(layout.Node);
			ImU32 outline = IM_COL32(20, 20, 22, 255);
			switch (mark)
			{
				case NodeOutline::None:     break;
				case NodeOutline::Warning:  outline = IM_COL32(232, 178, 72, 255); break;
				case NodeOutline::Error:    outline = IM_COL32(236, 86, 86, 255); break;
				case NodeOutline::Selected: outline = IM_COL32(250, 250, 250, 255); break;
			}
			draw.AddRect(Utils::ToImVec2(boxMinimum), Utils::ToImVec2(boxMaximum), outline, rounding, ImDrawFlags_None, mark == NodeOutline::None ? 1.0f : 2.0f);
			float liveY = boxMaximum.y + 3.0f * zoom;
			for (size_t line = 0; line < liveLines.size(); ++line)
			{
				const ImU32 color = line < liveValueCount ? IM_COL32(126, 214, 160, 255) : IM_COL32(236, 110, 110, 255);
				Utils::DrawCanvasText(draw, glm::vec2(boxMinimum.x + m_Metrics.Padding * zoom * 0.5f, liveY), fontSize, color, liveLines[line], maximum.x);
				liveY += fontSize + 2.0f * zoom;
			}

			for (size_t pinIndex = 0; pinIndex < layout.Pins.size() && pinIndex < shape.Pins.size(); ++pinIndex)
			{
				const GraphPinLayout& pin = layout.Pins[pinIndex];
				const glm::vec2 center = toScreen(pin.Center + offset);
				const float radius = m_Metrics.PinRadius * zoom;
				// A pin the node's kind no longer has (M24's review) is drawn in the error colour, its label too.
				const bool stale = m_StalePins.contains(GraphPinRef{ .Node = layout.Node, .Pin = pin.Name });
				const ImU32 color = stale ? StalePinColor : Utils::GraphPinColor(pin.Type);
				const ImU32 labelColor = stale ? StalePinColor : IM_COL32(214, 214, 218, 255);
				const bool linked = m_LinkedPins.contains(GraphPinRef{ .Node = layout.Node, .Pin = pin.Name });
				if (pin.Type == GraphPinType::Exec)
				{
					const ImVec2 a(center.x - radius, center.y - radius);
					const ImVec2 b(center.x + radius, center.y);
					const ImVec2 c(center.x - radius, center.y + radius);
					if (linked)
						draw.AddTriangleFilled(a, b, c, color);
					else
						draw.AddTriangle(a, b, c, color, 1.5f);
				}
				else if (linked)
					draw.AddCircleFilled(Utils::ToImVec2(center), radius, color);
				else
					draw.AddCircle(Utils::ToImVec2(center), radius, color, 0, 1.5f);
				const std::string& label = shape.Pins[pinIndex].Label;
				const float labelY = center.y - fontSize * 0.5f;
				if (pin.Direction == GraphPinDirection::Input)
					Utils::DrawCanvasText(draw, glm::vec2(center.x + radius + m_Metrics.LabelGap * zoom, labelY), fontSize, labelColor, label, boxMaximum.x);
				else
				{
					const float width = Utils::MeasureCanvasText(fontSize, label);
					Utils::DrawCanvasText(draw, glm::vec2(center.x - radius - m_Metrics.LabelGap * zoom - width, labelY), fontSize, labelColor, label, center.x);
				}
			}
		}

		// A stale pin's tooltip says why it is an error and how to remove what it holds.
		if (hovered && m_Drag == CanvasDrag::None && !m_StalePins.empty())
		{
			const glm::vec2 mouse = CanvasToGraph(m_View, Utils::ToVec2(ImGui::GetIO().MousePos) - minimum);
			const float pickRadius = std::max(m_Metrics.PinRadius * 2.0f, 6.0f / m_View.Zoom);
			if (const std::optional<GraphPinHit> pin = FindGraphPinAt(m_Layouts, mouse, pickRadius))
			{
				if (const auto stale = m_StalePins.find(GraphPinRef{ .Node = pin->Node, .Pin = pin->Pin }); stale != m_StalePins.end())
					ImGui::SetTooltip("%s", stale->second.c_str());
			}
		}

		// The gesture in progress: a link following the mouse, or the selection rectangle.
		if (m_Drag == CanvasDrag::Link)
		{
			const glm::vec2 mouse = toScreen(m_DragCurrent);
			const GraphPinRef anchor = m_Detached.has_value() ? m_Detached->From : GraphPinRef{ .Node = m_LinkFrom->Node, .Pin = m_LinkFrom->Pin };
			const bool fromOutput = m_Detached.has_value() || m_LinkFrom->Direction == GraphPinDirection::Output;
			if (const GraphPinLayout* pin = FindGraphPinLayout(m_Layouts, anchor))
			{
				const glm::vec2 start = toScreen(pin->Center);
				const auto curve = fromOutput ? GetGraphLinkCurve(start, mouse) : GetGraphLinkCurve(mouse, start);
				draw.AddBezierCubic(Utils::ToImVec2(curve[0]), Utils::ToImVec2(curve[1]), Utils::ToImVec2(curve[2]), Utils::ToImVec2(curve[3]),
					Utils::GraphPinColor(pin->Type), thickness);
			}
		}
		else if (m_Drag == CanvasDrag::Box)
		{
			const glm::vec2 a = toScreen(m_DragStart);
			const glm::vec2 b = toScreen(m_DragCurrent);
			draw.AddRectFilled(Utils::ToImVec2(glm::min(a, b)), Utils::ToImVec2(glm::max(a, b)), IM_COL32(110, 150, 230, 40));
			draw.AddRect(Utils::ToImVec2(glm::min(a, b)), Utils::ToImVec2(glm::max(a, b)), IM_COL32(110, 150, 230, 200));
		}
		if (m_Read->Document.Nodes.empty())
		{
			const char* hint = "Right-click the canvas, or use Add node, to add the graph's first node.";
			const ImVec2 hintSize = ImGui::CalcTextSize(hint);
			draw.AddText(ImVec2(minimum.x + (size.x - hintSize.x) * 0.5f, minimum.y + (size.y - hintSize.y) * 0.5f), IM_COL32(150, 150, 156, 255), hint);
		}
		draw.PopClipRect();
		HandleCanvasInput(context, minimum, hovered);
	}

	void GraphEditorPanel::HandleCanvasInput(EditorPanelContext& context, const glm::vec2& canvasMinimum, bool hovered)
	{
		ImGuiIO& io = ImGui::GetIO();
		const glm::vec2 canvasMouse = Utils::ToVec2(io.MousePos) - canvasMinimum;
		const glm::vec2 mouse = CanvasToGraph(m_View, canvasMouse);
		const bool editable = !context.Editor.IsReadOnly() && !context.Editor.GetPlay().IsTestRunActive() && m_Read.has_value();
		const float pickRadius = std::max(m_Metrics.PinRadius * 2.0f, 6.0f / m_View.Zoom);

		// Zoom about the mouse; pan with the middle button, or the right button once it moves.
		if (hovered && io.MouseWheel != 0.0f)
			m_View = ZoomGraphCanvas(m_View, canvasMouse, m_View.Zoom * std::pow(1.15f, io.MouseWheel));
		constexpr float RightDragThreshold = 4.0f;
		const bool panning = ImGui::IsItemActive()
			&& (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, RightDragThreshold));
		if (panning)
			m_View.Origin -= Utils::ToVec2(io.MouseDelta) / m_View.Zoom;
		if (!m_Read.has_value())
			return;

		if (m_Drag != CanvasDrag::None)
		{
			m_DragCurrent = mouse;
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				FinishDrag(context, mouse);
			return;
		}

		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			m_DragStart = mouse;
			m_DragCurrent = mouse;
			const bool additive = io.KeyCtrl || io.KeyShift;
			if (io.KeyAlt)
			{
				// Alt+click on a link removes it.
				if (const std::optional<GraphLink> link = FindGraphLinkAt(m_Read->Document.Links, m_Layouts, mouse, pickRadius); link && editable)
					Submit(context, MakeGraphEditorRequest(context.Editor.GetTypeRegistry(), GraphDisconnectParams{ .Graph = m_Reference, .From = link->From, .To = link->To }));
				return;
			}
			if (const std::optional<GraphPinHit> pin = FindGraphPinAt(m_Layouts, mouse, pickRadius); pin && editable)
			{
				// From a linked input, the drag carries its link away (released on empty canvas it is removed); from any other
				// pin it draws a new link.
				m_LinkFrom = pin;
				m_Detached.reset();
				if (pin->Direction == GraphPinDirection::Input)
				{
					const GraphPinRef input{ .Node = pin->Node, .Pin = pin->Pin };
					const auto linked = std::ranges::find(m_Read->Document.Links, input, &GraphLink::To);
					if (linked != m_Read->Document.Links.end())
						m_Detached = *linked;
				}
				m_Drag = CanvasDrag::Link;
				return;
			}
			if (const uint32_t node = FindGraphNodeAt(m_Layouts, mouse); node != 0)
			{
				const bool selected = std::ranges::binary_search(m_Selection, node);
				if (io.KeyCtrl && selected)
				{
					std::vector<uint32_t> remaining = m_Selection;
					std::erase(remaining, node);
					Select(std::move(remaining));
					return;
				}
				if (!selected)
				{
					std::vector<uint32_t> nodes = additive ? m_Selection : std::vector<uint32_t>{};
					nodes.push_back(node);
					Select(std::move(nodes));
				}
				m_Drag = editable ? CanvasDrag::Move : CanvasDrag::None;
				return;
			}
			m_AddToSelection = additive;
			if (!additive)
				Select({});
			m_Drag = CanvasDrag::Box;
			return;
		}

		// Right-click (not a pan): a link or a node's actions, else the node menu at the mouse.
		if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < RightDragThreshold * RightDragThreshold)
		{
			m_MenuPosition = mouse;
			m_MenuLink.reset();
			m_ContextLink.reset();
			if (const std::optional<GraphLink> link = FindGraphLinkAt(m_Read->Document.Links, m_Layouts, mouse, pickRadius))
			{
				m_ContextLink = link;
				m_OpenContextMenu = true;
			}
			else if (const uint32_t node = FindGraphNodeAt(m_Layouts, mouse); node != 0)
			{
				if (!std::ranges::binary_search(m_Selection, node))
					Select({ node });
				m_OpenContextMenu = true;
			}
			else if (editable && !m_Library.empty())
				m_OpenNodeMenu = true;
		}
	}

	void GraphEditorPanel::FinishDrag(EditorPanelContext& context, const glm::vec2& mouse)
	{
		const TypeRegistry& types = context.Editor.GetTypeRegistry();
		const CanvasDrag drag = std::exchange(m_Drag, CanvasDrag::None);
		const float pickRadius = std::max(m_Metrics.PinRadius * 2.0f, 6.0f / m_View.Zoom);
		if (drag == CanvasDrag::Move)
		{
			const glm::vec2 delta = glm::round(mouse - m_DragStart);
			if (glm::length(delta) * m_View.Zoom < 2.0f || m_Selection.empty())
				return;
			GraphMoveNodesParams params{ .Graph = m_Reference, .Positions = {} };
			for (const uint32_t id : m_Selection)
			{
				const GraphNode* node = FindGraphNode(m_Read->Document, id);
				if (node == nullptr)
					continue;
				const auto pending = m_PendingPositions.find(id);
				const glm::vec2 from = pending != m_PendingPositions.end() ? pending->second : node->Position;
				const glm::vec2 to = glm::clamp(from + delta, glm::vec2(-MaxGraphPosition), glm::vec2(MaxGraphPosition));
				params.Positions.push_back({ .Node = id, .Position = to });
				m_PendingPositions[id] = to;
			}
			m_LayoutStale = true;
			Submit(context, MakeGraphEditorRequest(types, params));
			return;
		}
		if (drag == CanvasDrag::Box)
		{
			std::vector<uint32_t> nodes = FindGraphNodesInRect(m_Layouts, m_DragStart, mouse);
			if (m_AddToSelection)
				nodes.insert(nodes.end(), m_Selection.begin(), m_Selection.end());
			Select(std::move(nodes));
			return;
		}
		if (drag != CanvasDrag::Link || !m_LinkFrom.has_value())
			return;
		const GraphPinHit from = *std::exchange(m_LinkFrom, std::nullopt);
		const std::optional<GraphLink> detached = std::exchange(m_Detached, std::nullopt);
		const std::optional<GraphPinHit> target = FindGraphPinAt(m_Layouts, mouse, pickRadius);
		if (detached.has_value())
		{
			// A link carried off its input: dropped on empty canvas it goes; dropped on another input it moves there.
			if (target.has_value() && target->Node == from.Node && target->Pin == from.Pin)
				return;
			if (!target.has_value() && FindGraphNodeAt(m_Layouts, mouse) == 0)
			{
				Submit(context, MakeGraphEditorRequest(types, GraphDisconnectParams{ .Graph = m_Reference, .From = detached->From, .To = detached->To }));
				return;
			}
			// One edit.batch, so a refused connect (incompatible pins, a cycle) undoes the disconnect and the link stays.
			if (target.has_value() && target->Direction == GraphPinDirection::Input)
				Submit(context, MakeGraphMoveLinkRequest(types, m_Reference, *detached, GraphPinRef{ .Node = target->Node, .Pin = target->Pin }));
			return;
		}
		if (target.has_value())
		{
			if (target->Node == from.Node || target->Direction == from.Direction)
				return;
			const GraphPinRef fromPin{ .Node = from.Node, .Pin = from.Pin };
			const GraphPinRef toPin{ .Node = target->Node, .Pin = target->Pin };
			const bool fromOutput = from.Direction == GraphPinDirection::Output;
			Submit(context, MakeGraphEditorRequest(types, GraphConnectParams{ .Graph = m_Reference, .From = fromOutput ? fromPin : toPin, .To = fromOutput ? toPin : fromPin }));
			return;
		}
		if (FindGraphNodeAt(m_Layouts, mouse) == 0 && !m_Library.empty())
		{
			// Dropped on empty canvas: the menu lists the kinds the link can join, and adds the chosen one linked (AddLinkedNode).
			m_MenuPosition = mouse;
			m_MenuLink = from;
			m_OpenNodeMenu = true;
		}
	}

	void GraphEditorPanel::HandleShortcuts(EditorPanelContext& context)
	{
		if (!m_Read.has_value() || ImGui::GetIO().WantTextInput || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
			|| ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
			return;
		const bool editable = !context.Editor.IsReadOnly() && !context.Editor.GetPlay().IsTestRunActive();
		if (ImGui::Shortcut(ImGuiKey_F))
			Frame(m_Selection, m_CanvasSize);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C) && !m_Selection.empty())
			m_Clipboard = CopyGraphNodes(m_Read->Document, m_Selection);
		if (!editable)
			return;
		if (ImGui::Shortcut(ImGuiKey_Delete) && !m_Selection.empty())
			RemoveSelection(context);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D) && !m_Selection.empty())
			DuplicateNodes(context, CopyGraphNodes(m_Read->Document, m_Selection), glm::vec2(32.0f), false);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V) && m_Clipboard.has_value() && !m_Clipboard->Nodes.empty())
		{
			// At the mouse when it is over the canvas, else beside the copied nodes.
			glm::vec2 first(std::numeric_limits<float>::max());
			for (const GraphNode& node : m_Clipboard->Nodes)
				first = glm::min(first, node.Position);
			const glm::vec2 canvasMouse = Utils::ToVec2(ImGui::GetIO().MousePos) - m_CanvasMinimum;
			const bool inside = canvasMouse.x >= 0.0f && canvasMouse.y >= 0.0f && canvasMouse.x < m_CanvasSize.x && canvasMouse.y < m_CanvasSize.y;
			const glm::vec2 offset = inside ? CanvasToGraph(m_View, canvasMouse) - first : glm::vec2(32.0f);
			DuplicateNodes(context, *m_Clipboard, offset, true);
		}
	}

	void GraphEditorPanel::DrawNodeMenu(EditorPanelContext& context)
	{
		if (std::exchange(m_OpenNodeMenu, false))
		{
			m_MenuSearch.clear();
			ImGui::OpenPopup("##GraphNodeMenu");
		}
		const float scale = Utils::EditorUiScale();
		ImGui::SetNextWindowSize(ImVec2(340.0f * scale, 420.0f * scale), ImGuiCond_Appearing);
		if (!ImGui::BeginPopup("##GraphNodeMenu"))
			return;
		const GraphPinDescription* link = m_MenuLink.has_value() ? FindPinDescription(GraphPinRef{ .Node = m_MenuLink->Node, .Pin = m_MenuLink->Pin }) : nullptr;
		if (m_MenuLink.has_value() && link == nullptr)
		{
			ImGui::TextDisabled("The link's pin is not in the node library.");
			ImGui::EndPopup();
			return;
		}
		Utils::EditorSectionHeading(link != nullptr ? "Add a linked node" : "Add a node");
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(-1.0f);
		const bool entered = ImGui::InputTextWithHint("##GraphNodeSearch", "Search kinds and titles...", &m_MenuSearch, ImGuiInputTextFlags_EnterReturnsTrue);
		const std::vector<GraphNodeMenuItem> items = FilterGraphNodeMenu(m_Library, m_MenuSearch, link);
		std::optional<size_t> chosen;
		if (entered && !items.empty())
			chosen = 0;
		if (ImGui::BeginChild("##GraphNodeMenuItems", ImVec2(0.0f, 0.0f)))
		{
			// One row per category heading and per kind, clipped so only the visible rows are drawn.
			struct Row
			{
				size_t Item = 0;
				bool Heading = false;
			};
			std::vector<Row> rows;
			for (size_t index = 0; index < items.size(); ++index)
			{
				if (index == 0 || items[index].Node->Category != items[index - 1].Node->Category)
					rows.push_back({ .Item = index, .Heading = true });
				rows.push_back({ .Item = index, .Heading = false });
			}
			if (items.empty())
				ImGui::TextDisabled("No kind matches.");
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(rows.size()));
			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
				{
					const Row& entry = rows[static_cast<size_t>(row)];
					const GraphNodeMenuItem& item = items[entry.Item];
					if (entry.Heading)
					{
						ImGui::TextDisabled("%s", std::string(GetGraphNodeCategoryLabel(item.Node->Category)).c_str());
						continue;
					}
					ImGui::PushID(item.Node->Kind.c_str());
					const std::string label = item.Node->Title.empty() ? item.Node->Kind : item.Node->Title;
					if (ImGui::Selectable(label.c_str()))
						chosen = entry.Item;
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s\n%s", item.Node->Kind.c_str(), item.Node->Description.c_str());
					ImGui::PopID();
				}
			}
		}
		ImGui::EndChild();
		if (chosen.has_value())
		{
			AddNode(context, *items[*chosen].Node, items[*chosen].Pin);
			m_MenuLink.reset();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void GraphEditorPanel::DrawContextMenu(EditorPanelContext& context)
	{
		if (std::exchange(m_OpenContextMenu, false))
			ImGui::OpenPopup("##GraphContextMenu");
		if (!ImGui::BeginPopup("##GraphContextMenu"))
			return;
		const bool editable = !context.Editor.IsReadOnly() && !context.Editor.GetPlay().IsTestRunActive() && m_Read.has_value();
		if (m_ContextLink.has_value())
		{
			if (ImGui::MenuItem("Disconnect", "Alt+click", false, editable))
				Submit(context, MakeGraphEditorRequest(context.Editor.GetTypeRegistry(), GraphDisconnectParams{ .Graph = m_Reference, .From = m_ContextLink->From, .To = m_ContextLink->To }));
		}
		else if (m_Read.has_value())
		{
			if (ImGui::MenuItem("Delete", "Delete", false, editable && !m_Selection.empty()))
				RemoveSelection(context);
			if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editable && !m_Selection.empty()))
				DuplicateNodes(context, CopyGraphNodes(m_Read->Document, m_Selection), glm::vec2(32.0f), false);
			if (ImGui::MenuItem("Copy", "Ctrl+C", false, !m_Selection.empty()))
				m_Clipboard = CopyGraphNodes(m_Read->Document, m_Selection);
			if (ImGui::MenuItem("Paste", "Ctrl+V", false, editable && m_Clipboard.has_value()))
			{
				glm::vec2 first(std::numeric_limits<float>::max());
				for (const GraphNode& node : m_Clipboard->Nodes)
					first = glm::min(first, node.Position);
				DuplicateNodes(context, *m_Clipboard, m_MenuPosition - first, true);
			}
			if (ImGui::MenuItem("Frame", "F"))
				Frame(m_Selection, m_CanvasSize);
		}
		ImGui::EndPopup();
	}

	void GraphEditorPanel::DrawSidePanel(EditorPanelContext& context)
	{
		if (!m_Read.has_value())
		{
			ImGui::TextDisabled("Reading the graph...");
			return;
		}
		DrawMembers(context);
		ImGui::Spacing();
		DrawNodeDetails(context);
	}

	std::optional<Json> GraphEditorPanel::DrawValueField(EditorPanelContext& context, const std::string& key, std::string_view label, uint8_t widgetValue,
		const Json& stored, std::span<const std::string> choices, std::string_view assetType)
	{
		// The label shows beside the field, which keeps its id whatever the label says.
		const std::string widgetLabel = std::format("{}###Value", label);
		const auto widget = static_cast<GraphValueWidget>(widgetValue);
		const bool editing = m_FieldEdit.has_value() && m_FieldEdit->Key == key;
		Json value = editing ? m_FieldEdit->Value : stored;
		bool changed = false;
		bool commit = false;
		ImGui::PushID(key.c_str());
		ImGui::SetNextItemWidth(label.empty() ? -1.0f : ImGui::GetContentRegionAvail().x * 0.6f);
		switch (widget)
		{
			case GraphValueWidget::None:
				break;
			case GraphValueWidget::Bool:
			{
				bool checked = value.is_boolean() && JsonReader(value).ReadBool().value_or(false);
				if (ImGui::Checkbox(widgetLabel.c_str(), &checked))
				{
					value = checked;
					changed = commit = true;
				}
				break;
			}
			case GraphValueWidget::Number:
			{
				float number = Utils::ReadGraphFloat(value, 0.0f);
				if (ImGui::DragFloat(widgetLabel.c_str(), &number, 0.05f, 0.0f, 0.0f, "%.4g"))
				{
					value = Utils::ToGraphNumber(number);
					changed = true;
				}
				break;
			}
			case GraphValueWidget::Integer:
			{
				int number = value.is_number() ? JsonReader(value).ReadInt32().value_or(0) : 0;
				if (ImGui::DragInt(widgetLabel.c_str(), &number))
				{
					value = number;
					changed = true;
				}
				break;
			}
			case GraphValueWidget::String:
			{
				std::string text = value.is_string() ? JsonReader(value).ReadString().value_or(std::string()) : std::string();
				if (ImGui::InputText(widgetLabel.c_str(), &text))
				{
					value = text;
					changed = true;
				}
				break;
			}
			case GraphValueWidget::Vector:
			{
				std::array<float, 3> components = Utils::ReadGraphFloats<3>(value, { 0.0f, 0.0f, 0.0f });
				if (ImGui::DragFloat3(widgetLabel.c_str(), components.data(), 0.05f, 0.0f, 0.0f, "%.4g"))
				{
					value = Utils::WriteGraphFloats(components);
					changed = true;
				}
				break;
			}
			case GraphValueWidget::Color:
			{
				std::array<float, 4> components = Utils::ReadGraphFloats<4>(value, { 1.0f, 1.0f, 1.0f, 1.0f });
				if (ImGui::ColorEdit4(widgetLabel.c_str(), components.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
				{
					value = Utils::WriteGraphFloats(components);
					changed = true;
				}
				break;
			}
			case GraphValueWidget::Quat:
			{
				std::array<float, 4> components = Utils::ReadGraphFloats<4>(value, { 0.0f, 0.0f, 0.0f, 1.0f });
				if (ImGui::DragFloat4(widgetLabel.c_str(), components.data(), 0.01f, -1.0f, 1.0f, "%.4g"))
				{
					value = Utils::WriteGraphFloats(components);
					changed = true;
				}
				break;
			}
			case GraphValueWidget::Choice:
			{
				const std::string current = value.is_string() ? JsonReader(value).ReadString().value_or(std::string()) : std::string();
				if (ImGui::BeginCombo(widgetLabel.c_str(), current.c_str()))
				{
					for (const std::string& choice : choices)
					{
						if (ImGui::Selectable(choice.c_str(), choice == current))
						{
							value = choice;
							changed = commit = true;
						}
					}
					ImGui::EndCombo();
				}
				break;
			}
			case GraphValueWidget::Asset:
			{
				// An Asset literal is an asset handle (decision 7), picked among the assets of the pin's type.
				const AssetRegistry& registry = context.Editor.GetAssets().GetRegistry();
				const std::optional<AssetHandle> current = value.is_string() ? AssetHandle::FromString(JsonReader(value).ReadString().value_or(std::string()))
																			 : std::nullopt;
				const std::string preview = current.has_value() ? context.Editor.GetAssets().GetReferencePath(*current) : std::string("None");
				if (ImGui::BeginCombo(widgetLabel.c_str(), preview.c_str(), ImGuiComboFlags_HeightLarge))
				{
					if (ImGui::Selectable("None", !current.has_value()))
					{
						value = Json();
						changed = commit = true;
					}
					std::vector<std::pair<std::string, AssetHandle>> assets;
					for (const AssetHandle handle : registry.GetHandles())
					{
						const auto location = registry.Locate(handle);
						if (location.has_value() && (assetType.empty() || AssetTypeToString(location->Type) == assetType))
							assets.emplace_back(registry.GetReferencePath(handle), handle);
					}
					std::ranges::sort(assets);
					for (const auto& [path, handle] : assets)
					{
						if (ImGui::Selectable(path.c_str(), current == handle))
						{
							value = handle.ToString();
							changed = commit = true;
						}
					}
					ImGui::EndCombo();
				}
				break;
			}
			case GraphValueWidget::Any:
			{
				// Any scalar: JSON null, true, a number, or else the text as a string. While it is edited the candidate is the text.
				std::string text = stored.is_string() ? JsonReader(stored).ReadString().value_or(std::string()) : stored.dump();
				if (editing && m_FieldEdit->Value.is_string())
					text = JsonReader(m_FieldEdit->Value).ReadString().value_or(std::string());
				if (ImGui::InputText(widgetLabel.c_str(), &text))
				{
					m_FieldEdit = FieldEdit{ .Key = key, .Value = Json(text), .Active = ImGui::IsItemActive(), .Submitted = false };
					ImGui::PopID();
					return std::nullopt;
				}
				if (ImGui::IsItemDeactivatedAfterEdit() && editing)
				{
					const Json parsed = Utils::ParseAnyLiteral(text);
					m_FieldEdit = FieldEdit{ .Key = key, .Value = parsed, .Active = false, .Submitted = true };
					ImGui::PopID();
					return parsed;
				}
				if (editing)
					m_FieldEdit->Active = ImGui::IsItemActive();
				ImGui::PopID();
				return std::nullopt;
			}
		}
		const bool released = ImGui::IsItemDeactivatedAfterEdit();
		const bool active = ImGui::IsItemActive();
		ImGui::PopID();
		if (changed)
			m_FieldEdit = FieldEdit{ .Key = key, .Value = value, .Active = active, .Submitted = false };
		else if (editing)
			m_FieldEdit->Active = active;
		if ((commit || released) && m_FieldEdit.has_value() && m_FieldEdit->Key == key)
		{
			m_FieldEdit->Submitted = true;
			return m_FieldEdit->Value;
		}
		return std::nullopt;
	}

	void GraphEditorPanel::DrawMembers(EditorPanelContext& context)
	{
		EditorContext& editor = context.Editor;
		const TypeRegistry& types = editor.GetTypeRegistry();
		const bool editable = !editor.IsReadOnly() && !editor.GetPlay().IsTestRunActive();
		const GraphDocument& graph = m_Read->Document;
		Utils::EditorSectionHeading("Behaviour");
		ImGui::BeginDisabled(!editable);
		std::string name = graph.Name;
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText("##BehaviourName", &name, ImGuiInputTextFlags_EnterReturnsTrue) && name != graph.Name)
			Submit(context, MakeGraphEditorRequest(types, GraphSetMembersParams{ .Graph = m_Reference, .Name = name, .Rename = {}, .Remove = {}, .Fields = {}, .Variables = {} }));
		ImGui::SetItemTooltip("The behaviour's name (Script.Define), a Luau identifier. Press Enter to rename it.");
		// M39 (Docs/Decisions/0056-m39-graphs-behaviours.md decision 13): the world-pause opt-in.
		bool runs = graph.RunsWhileWorldPaused;
		if (ImGui::Checkbox("Runs while the world is paused", &runs))
			Submit(context, MakeGraphWorldPauseRequest(m_Reference, runs));
		ImGui::SetItemTooltip("Whether this behaviour updates while the world is paused (Time.PauseWorld), as a pause menu's does.");

		// One row per member: its name (Enter renames it, with its Self nodes), its kind, its default and its removal.
		const auto drawMember = [this, &context, &types](const std::string& memberName, GraphValueKind kind, const Json& stored, std::span<const std::string> choices,
									const std::function<void(const Json&)>& replace)
		{
			ImGui::PushID(memberName.c_str());
			std::string renamed = memberName;
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
			if (ImGui::InputText("##Name", &renamed, ImGuiInputTextFlags_EnterReturnsTrue) && renamed != memberName)
				Submit(context, MakeGraphEditorRequest(types, GraphSetMembersParams{ .Graph = m_Reference, .Name = {}, .Rename = { GraphMemberRename{ .From = memberName, .To = renamed } }, .Remove = {}, .Fields = {}, .Variables = {} }));
			ImGui::SameLine();
			ImGui::TextDisabled("%s", std::string(Utils::GetGraphValueKindName(kind)).c_str());
			ImGui::SameLine();
			if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f)))
				Submit(context, MakeGraphEditorRequest(types, GraphSetMembersParams{ .Graph = m_Reference, .Name = {}, .Rename = {}, .Remove = { memberName }, .Fields = {}, .Variables = {} }));
			ImGui::SetItemTooltip("Remove this member (refused while a node uses it).");
			const auto widget = Utils::GetMemberWidget(kind);
			if (widget != GraphValueWidget::None)
			{
				if (const std::optional<Json> value = DrawValueField(context, "member:" + memberName, {}, std::to_underlying(widget), stored, choices, {}))
					replace(*value);
			}
			// Decision 14: in play, the instance's value of the member (graph.liveValues' members, sorted by name).
			if (const GraphLiveValuesResult* live = FindShownLiveValues())
			{
				const auto found = std::ranges::lower_bound(live->Members, memberName, {}, &GraphLiveMember::Name);
				if (found != live->Members.end() && found->Name == memberName)
					ImGui::TextColored(ImVec4(0.49f, 0.84f, 0.63f, 1.0f), "%s", GraphLiveView::FormatMemberLine(*found).c_str());
			}
			ImGui::PopID();
		};

		Utils::EditorSectionHeading("Fields");
		for (const GraphField& field : graph.Fields)
		{
			drawMember(field.Name, field.Kind, field.Default.Get(), field.EnumValues, [this, &context, &types, &field](const Json& value)
			{
				GraphField replaced = field;
				replaced.Default = VariantValue(value);
				Submit(context, MakeGraphEditorRequest(types, GraphSetMembersParams{ .Graph = m_Reference, .Name = {}, .Rename = {}, .Remove = {}, .Fields = { replaced }, .Variables = {} }));
			});
			DrawFieldOptions(context, field);
		}
		if (graph.Fields.empty())
			ImGui::TextDisabled("No fields: set per script slot in the inspector.");
		Utils::EditorSectionHeading("Variables");
		for (const GraphVariable& variable : graph.Variables)
		{
			drawMember(variable.Name, variable.Kind, variable.Default.Get(), variable.EnumValues, [this, &context, &types, &variable](const Json& value)
			{
				GraphVariable replaced = variable;
				replaced.Default = VariantValue(value);
				Submit(context, MakeGraphEditorRequest(types, GraphSetMembersParams{ .Graph = m_Reference, .Name = {}, .Rename = {}, .Remove = {}, .Fields = {}, .Variables = { replaced } }));
			});
		}
		if (graph.Variables.empty())
			ImGui::TextDisabled("No variables: instance state OnCreate sets.");

		// A name the graph already holds adds nothing: graph.setMembers would replace that member (or refuse the name as another
		// member's), so a member is renamed or edited in its own row instead (phase 6's landing, as Add method's guard).
		const auto holds = [&graph](const std::string& memberName)
		{
			return std::ranges::find(graph.Fields, memberName, &GraphField::Name) != graph.Fields.end()
				|| std::ranges::find(graph.Variables, memberName, &GraphVariable::Name) != graph.Variables.end()
				|| std::ranges::find(graph.Methods, memberName, &GraphMethod::Name) != graph.Methods.end();
		};
		constexpr const char* HeldNameTooltip = "A field, variable or method of the graph has this name: edit that one in its row.";

		// A new member: its name, kind (an Asset's type, an Enum's choices, an Array's element) and whether it is a field.
		ImGui::Separator();
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##NewMemberName", "New member name", &m_NewMemberName);
		std::array<const char*, GraphValueKindNames.size()> kindLabels{};
		for (size_t index = 0; index < kindLabels.size(); ++index)
			kindLabels[index] = GraphValueKindNames[index].data();
		// A field's kinds, and a variable's, Any (M39) included.
		const int kindCount = m_NewMemberVariable ? static_cast<int>(kindLabels.size()) : FieldKindCount;
		m_NewMemberKind = std::clamp(m_NewMemberKind, 0, kindCount - 1);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::Combo("##NewMemberKind", &m_NewMemberKind, kindLabels.data(), kindCount);
		ImGui::SameLine();
		ImGui::Checkbox("Variable", &m_NewMemberVariable);
		ImGui::SetItemTooltip("A variable is instance state OnCreate sets; a field is set per script slot in the inspector.");
		const auto kind = static_cast<GraphValueKind>(std::clamp(m_NewMemberKind, 0, kindCount - 1));
		const std::vector<std::string_view> assetTypes = Utils::GetGraphAssetTypeNames();
		GraphValueKind element = GraphValueKind::Number;
		if (kind == GraphValueKind::Array)
		{
			// Any kind but Array and Any (GraphDocument.h).
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::Combo("##NewMemberElement", &m_NewMemberElement, kindLabels.data(), ElementKindCount);
			element = static_cast<GraphValueKind>(std::clamp(m_NewMemberElement, 0, ElementKindCount - 1));
		}
		const GraphValueKind typed = kind == GraphValueKind::Array ? element : kind;
		if (typed == GraphValueKind::Asset)
		{
			m_NewMemberAssetType = std::clamp(m_NewMemberAssetType, 0, static_cast<int>(assetTypes.size()) - 1);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::BeginCombo("##NewMemberAssetType", std::string(assetTypes[static_cast<size_t>(m_NewMemberAssetType)]).c_str()))
			{
				for (size_t index = 0; index < assetTypes.size(); ++index)
					if (ImGui::Selectable(std::string(assetTypes[index]).c_str(), static_cast<int>(index) == m_NewMemberAssetType))
						m_NewMemberAssetType = static_cast<int>(index);
				ImGui::EndCombo();
			}
		}
		if (typed == GraphValueKind::Enum)
		{
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputTextWithHint("##NewMemberChoices", "Choices: A, B, C", &m_NewMemberChoices);
		}
		const std::vector<std::string> choices = typed == GraphValueKind::Enum ? Utils::SplitGraphChoices(m_NewMemberChoices) : std::vector<std::string>{};
		const bool memberTaken = holds(m_NewMemberName);
		ImGui::BeginDisabled(!IsGraphIdentifier(m_NewMemberName) || memberTaken || (typed == GraphValueKind::Enum && choices.empty()));
		if (ImGui::Button(m_NewMemberVariable ? "Add variable" : "Add field", ImVec2(-1.0f, 0.0f)))
		{
			const std::string assetType = typed == GraphValueKind::Asset ? std::string(assetTypes[static_cast<size_t>(m_NewMemberAssetType)]) : std::string();
			const VariantValue defaultValue(Utils::MakeMemberDefault(kind, choices));
			GraphSetMembersParams params{ .Graph = m_Reference, .Name = {}, .Rename = {}, .Remove = {}, .Fields = {}, .Variables = {} };
			if (m_NewMemberVariable)
				params.Variables.push_back(GraphVariable{ .Name = m_NewMemberName, .Kind = kind, .Default = defaultValue, .Element = element, .AssetType = assetType, .EnumValues = choices });
			else
				params.Fields.push_back(GraphField{ .Name = m_NewMemberName, .Kind = kind, .Default = defaultValue, .Element = element, .AssetType = assetType, .EnumValues = choices, .Options = {} });
			Submit(context, MakeGraphEditorRequest(types, params));
			m_NewMemberName.clear();
		}
		ImGui::EndDisabled();
		if (memberTaken)
			ImGui::SetItemTooltip("%s", HeldNameTooltip);

		// M39 (decision 13): the methods, each with its parameters and result, and a new method's name.
		Utils::EditorSectionHeading("Methods");
		for (const GraphMethod& method : graph.Methods)
			DrawMethod(context, method);
		if (graph.Methods.empty())
			ImGui::TextDisabled("No methods: scripts call them on the instance.");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##NewMethodName", "New method name", &m_NewMethodName);
		const bool taken = holds(m_NewMethodName);
		ImGui::BeginDisabled(!IsGraphMethodName(m_NewMethodName) || taken);
		if (ImGui::Button("Add method", ImVec2(-1.0f, 0.0f)))
		{
			Submit(context, MakeGraphMethodRequest(types, m_Reference, GraphMethod{ .Name = m_NewMethodName, .Parameters = {}, .Results = {} }));
			m_NewMethodName.clear();
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("%s", taken ? HeldNameTooltip : "Adds a method without parameters or a result; scripts call it on the instance.");
		ImGui::EndDisabled();
	}

	void GraphEditorPanel::DrawMethod(EditorPanelContext& context, const GraphMethod& method)
	{
		// One method (M39, decision 13): its name (Enter renames it with its Method and Return nodes), its signature and its
		// removal; then each parameter's name (Enter renames it), kind, an Asset's type and removal, Add parameter, and the
		// result. Every change but a rename or removal submits the method's new declaration through graph.setMembers.
		const TypeRegistry& types = context.Editor.GetTypeRegistry();
		const auto replace = [this, &context, &types](const GraphMethod& replaced)
		{
			Submit(context, MakeGraphMethodRequest(types, m_Reference, replaced));
		};
		const std::vector<std::string_view> assetTypes = Utils::GetGraphAssetTypeNames();
		std::array<const char*, MethodValueKinds.size()> kindLabels{};
		for (size_t index = 0; index < kindLabels.size(); ++index)
			kindLabels[index] = GraphValueKindNames[std::to_underlying(MethodValueKinds[index])].data();
		const auto kindIndex = [](GraphValueKind kind)
		{
			const auto found = std::ranges::find(MethodValueKinds, kind);
			return static_cast<int>(found != MethodValueKinds.end() ? found - MethodValueKinds.begin() : MethodValueKinds.size() - 1);
		};
		// An Asset's type, "(any)" first; true when it changed `assetType`.
		const auto drawAssetType = [&assetTypes](std::string& assetType)
		{
			bool changed = false;
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::BeginCombo("##AssetType", assetType.empty() ? "(any asset)" : assetType.c_str()))
			{
				if (ImGui::Selectable("(any asset)", assetType.empty()))
				{
					changed = !assetType.empty();
					assetType.clear();
				}
				for (const std::string_view type : assetTypes)
				{
					if (ImGui::Selectable(std::string(type).c_str(), assetType == type))
					{
						changed = changed || assetType != type;
						assetType = std::string(type);
					}
				}
				ImGui::EndCombo();
			}
			return changed;
		};

		ImGui::PushID(method.Name.c_str());
		std::string renamed = method.Name;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
		if (ImGui::InputText("##Name", &renamed, ImGuiInputTextFlags_EnterReturnsTrue) && renamed != method.Name)
		{
			Submit(context,
				MakeGraphEditorRequest(types,
					GraphSetMembersParams{ .Graph = m_Reference,
						.Name = {},
						.Rename = { GraphMemberRename{ .From = method.Name, .To = renamed } },
						.Remove = {},
						.Fields = {},
						.Variables = {},
						.Methods = {},
						.RunsWhileWorldPaused = false }));
		}
		ImGui::SetItemTooltip(
			"The method's name. Press Enter to rename it with its Method and Return nodes; its callers name it by a string, which no rename "
			"rewrites.");
		ImGui::SameLine();
		ImGui::TextDisabled("%s", Utils::DescribeMethodSignature(method).c_str());
		ImGui::SameLine();
		if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f)))
		{
			Submit(context,
				MakeGraphEditorRequest(types,
					GraphSetMembersParams{ .Graph = m_Reference,
						.Name = {},
						.Rename = {},
						.Remove = { method.Name },
						.Fields = {},
						.Variables = {},
						.Methods = {},
						.RunsWhileWorldPaused = false }));
		}
		ImGui::SetItemTooltip("Remove this method (refused while a node uses it).");

		ImGui::Indent();
		for (size_t index = 0; index < method.Parameters.size(); ++index)
		{
			const GraphParameter& parameter = method.Parameters[index];
			ImGui::PushID(static_cast<int>(index));
			std::string parameterName = parameter.Name;
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
			if (ImGui::InputText("##Parameter", &parameterName, ImGuiInputTextFlags_EnterReturnsTrue) && parameterName != parameter.Name)
			{
				GraphMethod replaced = method;
				replaced.Parameters[index].Name = parameterName;
				replace(replaced);
			}
			ImGui::SetItemTooltip("The parameter's name; press Enter to rename it.");
			ImGui::SameLine();
			int selected = kindIndex(parameter.Kind);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
			if (ImGui::Combo("##Kind", &selected, kindLabels.data(), static_cast<int>(kindLabels.size())))
			{
				GraphMethod replaced = method;
				replaced.Parameters[index].Kind = MethodValueKinds[static_cast<size_t>(selected)];
				replaced.Parameters[index].AssetType.clear();
				replace(replaced);
			}
			ImGui::SameLine();
			if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f)))
			{
				GraphMethod replaced = method;
				replaced.Parameters.erase(replaced.Parameters.begin() + static_cast<std::ptrdiff_t>(index));
				replace(replaced);
			}
			ImGui::SetItemTooltip("Remove this parameter (refused while a link of the Method node's output needs it).");
			if (parameter.Kind == GraphValueKind::Asset)
			{
				std::string assetType = parameter.AssetType;
				if (drawAssetType(assetType))
				{
					GraphMethod replaced = method;
					replaced.Parameters[index].AssetType = assetType;
					replace(replaced);
				}
			}
			ImGui::PopID();
		}
		ImGui::BeginDisabled(method.Parameters.size() >= MaxGraphMethodParameters);
		if (ImGui::Button("Add parameter"))
		{
			GraphMethod replaced = method;
			replaced.Parameters.push_back(
				GraphParameter{ .Name = Utils::MakeParameterName(method), .Kind = GraphValueKind::Any, .AssetType = {} });
			replace(replaced);
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("A method takes at most 4 parameters, as Script.Call passes at most 4 arguments.");

		// The result: none, or one of a method's kinds.
		std::array<const char*, MethodValueKinds.size() + 1> resultLabels{};
		resultLabels[0] = "No result";
		for (size_t index = 0; index < kindLabels.size(); ++index)
			resultLabels[index + 1] = kindLabels[index];
		int result = method.Results.empty() ? 0 : kindIndex(method.Results.front().Kind) + 1;
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::Combo("##Result", &result, resultLabels.data(), static_cast<int>(resultLabels.size())))
		{
			GraphMethod replaced = method;
			replaced.Results.clear();
			if (result > 0)
				replaced.Results.push_back(GraphResult{ .Kind = MethodValueKinds[static_cast<size_t>(result - 1)], .AssetType = {} });
			replace(replaced);
		}
		ImGui::SetItemTooltip("What its Return nodes give back: none, or a value of this kind.");
		if (!method.Results.empty() && method.Results.front().Kind == GraphValueKind::Asset)
		{
			ImGui::PushID("Result");
			std::string assetType = method.Results.front().AssetType;
			if (drawAssetType(assetType))
			{
				GraphMethod replaced = method;
				replaced.Results.front().AssetType = assetType;
				replace(replaced);
			}
			ImGui::PopID();
		}
		ImGui::Unindent();
		ImGui::PopID();
	}

	void GraphEditorPanel::DrawFieldOptions(EditorPanelContext& context, const GraphField& field)
	{
		// The Luau FieldOptions a field is declared with: Min, Max and Step for a Number, Integer or Vector, and a Tooltip for
		// every kind whose Field constructor takes options (GraphDocument.h).
		const bool numeric = field.Kind == GraphValueKind::Number || field.Kind == GraphValueKind::Integer || field.Kind == GraphValueKind::Vector;
		const bool takesOptions = numeric || field.Kind == GraphValueKind::Bool || field.Kind == GraphValueKind::String || field.Kind == GraphValueKind::Color
			|| field.Kind == GraphValueKind::Quat;
		if (!takesOptions)
			return;
		ImGui::PushID(field.Name.c_str());
		if (!ImGui::TreeNodeEx("Options", ImGuiTreeNodeFlags_SpanAvailWidth))
		{
			ImGui::PopID();
			return;
		}
		const TypeRegistry& types = context.Editor.GetTypeRegistry();
		const auto replace = [this, &context, &types, &field](std::map<std::string, VariantValue> options)
		{
			GraphField replaced = field;
			replaced.Options = std::move(options);
			Submit(context, MakeGraphEditorRequest(types, GraphSetMembersParams{ .Graph = m_Reference, .Name = {}, .Rename = {}, .Remove = {}, .Fields = { replaced }, .Variables = {} }));
		};
		if (numeric)
		{
			// A bound added starts at the default (each component's least or greatest for a Vector), which it then holds.
			const Json& stored = field.Default.Get();
			float least = Utils::ReadGraphFloat(stored, 0.0f);
			float greatest = least;
			if (stored.is_array())
			{
				const std::array<float, 3> components = Utils::ReadGraphFloats<3>(stored, { 0.0f, 0.0f, 0.0f });
				least = std::min({ components[0], components[1], components[2] });
				greatest = std::max({ components[0], components[1], components[2] });
			}
			for (const std::string_view option : { std::string_view("Min"), std::string_view("Max"), std::string_view("Step") })
			{
				const std::string name(option);
				ImGui::PushID(name.c_str());
				const auto found = field.Options.find(name);
				if (found == field.Options.end())
				{
					if (ImGui::SmallButton(std::format("Add {}", name).c_str()))
					{
						std::map<std::string, VariantValue> options = field.Options;
						float initial = 1.0f; // a Step
						if (option == "Min")
							initial = least;
						else if (option == "Max")
							initial = greatest;
						options.insert_or_assign(name, VariantValue(Utils::ToGraphNumber(initial)));
						replace(std::move(options));
					}
				}
				else
				{
					if (const std::optional<Json> value = DrawValueField(context, std::format("option:{}:{}", field.Name, name), name, std::to_underlying(GraphValueWidget::Number),
							found->second.Get(), {}, {}))
					{
						std::map<std::string, VariantValue> options = field.Options;
						options.insert_or_assign(name, VariantValue(*value));
						replace(std::move(options));
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("x"))
					{
						std::map<std::string, VariantValue> options = field.Options;
						options.erase(name);
						replace(std::move(options));
					}
				}
				ImGui::PopID();
			}
		}
		const auto tooltip = field.Options.find("Tooltip");
		const Json stored = tooltip != field.Options.end() ? tooltip->second.Get() : Json("");
		if (const std::optional<Json> value = DrawValueField(context, std::format("option:{}:Tooltip", field.Name), "Tooltip", std::to_underlying(GraphValueWidget::String), stored, {}, {}))
		{
			// An empty tooltip is none.
			std::map<std::string, VariantValue> options = field.Options;
			if (value->is_string() && JsonReader(*value).ReadString().value_or(std::string()).empty())
				options.erase("Tooltip");
			else
				options.insert_or_assign("Tooltip", VariantValue(*value));
			replace(std::move(options));
		}
		ImGui::TreePop();
		ImGui::PopID();
	}

	void GraphEditorPanel::DrawNodeDetails(EditorPanelContext& context)
	{
		EditorContext& editor = context.Editor;
		Utils::EditorSectionHeading("Node");
		if (m_Selection.size() != 1)
		{
			if (m_Selection.empty())
				ImGui::TextDisabled("Select a node to edit its inputs.");
			else
				ImGui::TextDisabled("%zu nodes selected.", m_Selection.size());
			return;
		}
		const GraphNode* node = FindGraphNode(m_Read->Document, m_Selection.front());
		if (node == nullptr)
			return;
		const GraphNodeDescription* description = FindDescription(node->Kind);
		ImGui::TextUnformatted(description != nullptr && !description->Title.empty() ? description->Title.c_str() : node->Kind.c_str());
		ImGui::TextDisabled("%s  (node %u)", node->Kind.c_str(), node->Id);
		if (description != nullptr && !description->Description.empty())
			ImGui::TextWrapped("%s", description->Description.c_str());
		for (const GraphDiagnostic& diagnostic : m_Read->Diagnostics)
		{
			if (diagnostic.Node != node->Id)
				continue;
			const bool error = diagnostic.Severity == DiagnosticSeverity::Error && !diagnostic.Incomplete;
			ImGui::PushStyleColor(ImGuiCol_Text, error ? ImVec4(0.95f, 0.48f, 0.48f, 1.0f) : ImVec4(0.91f, 0.73f, 0.39f, 1.0f));
			ImGui::TextWrapped("%s", diagnostic.Message.c_str());
			ImGui::PopStyleColor();
		}
		if (description == nullptr)
		{
			ImGui::TextWrapped("The node library has no kind %s: remove the node, or restore the binding it was made from.", node->Kind.c_str());
			return;
		}
		const bool editable = !editor.IsReadOnly() && !editor.GetPlay().IsTestRunActive();
		const Result<const GraphNodeLibrary*> library = editor.GetGraphNodeLibrary();
		ImGui::BeginDisabled(!editable);
		for (const GraphPinDescription& pin : description->Pins)
		{
			if (pin.Direction != GraphPinDirection::Input || pin.Type == GraphPinType::Exec)
				continue;
			ImGui::PushID(pin.Name.c_str());
			const std::string typeName = pin.TypeName.empty() ? std::string(GraphPinTypeToString(pin.Type)) : pin.TypeName;
			const std::string typeText = std::format("{}{}", typeName, pin.Optional ? ", optional" : "");
			const auto literal = node->Values.find(pin.Name);
			const bool hasLiteral = literal != node->Values.end() && !literal->second.Get().is_null();
			if (m_LinkedPins.contains(GraphPinRef{ .Node = node->Id, .Pin = pin.Name }))
				ImGui::TextDisabled("%s: linked (%s)", pin.Name.c_str(), typeText.c_str());
			else if (Utils::GetPinWidget(pin.Type) == GraphValueWidget::None)
				ImGui::TextDisabled("%s: link a value (%s)", pin.Name.c_str(), typeText.c_str());
			else
			{
				std::vector<std::string> choices;
				if (pin.Type == GraphPinType::Enum && library.has_value())
				{
					const std::span<const std::string> enumChoices = (*library)->GetEnumChoices(pin.TypeName);
					choices.assign(enumChoices.begin(), enumChoices.end());
				}
				const Json stored = hasLiteral ? literal->second.Get() : pin.Default.Get();
				const std::string key = std::format("literal:{}:{}", node->Id, pin.Name);
				const std::string label = std::format("{} ({})", pin.Name, typeText);
				if (const std::optional<Json> value = DrawValueField(context, key, label, std::to_underlying(Utils::GetPinWidget(pin.Type)), stored, choices, pin.TypeName))
					SetLiteral(context, node->Id, pin.Name, *value);
				if (hasLiteral)
				{
					if (ImGui::SmallButton("Clear literal"))
						SetLiteral(context, node->Id, pin.Name, Json());
					ImGui::SetItemTooltip("Remove the literal: the input takes its default, or must be linked.");
				}
			}
			ImGui::PopID();
		}
		// M24's review: a literal on a pin the kind no longer has (a binding's parameter renamed or removed, decision 10) keeps the
		// graph from importing until it goes, which Clear literal does through graph.setValues with null.
		for (const auto& [pin, value] : node->Values)
		{
			const auto found = std::ranges::find(description->Pins, pin, &GraphPinDescription::Name);
			if (found != description->Pins.end() && found->Direction == GraphPinDirection::Input && found->Type != GraphPinType::Exec)
				continue;
			ImGui::PushID(std::format("stale:{}", pin).c_str());
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.48f, 0.48f, 1.0f));
			ImGui::TextWrapped("%s = %s: %s has no input %s now, so the graph does not import.", pin.c_str(), FormatGraphLiteral(value.Get()).c_str(),
				node->Kind.c_str(), pin.c_str());
			ImGui::PopStyleColor();
			if (ImGui::SmallButton("Clear literal"))
				SetLiteral(context, node->Id, pin, Json());
			ImGui::SetItemTooltip("Remove the literal the node's kind no longer takes.");
			ImGui::PopID();
		}
		ImGui::EndDisabled();
	}

}
