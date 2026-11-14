#include "EditorPCH.h"
#include "EditorCore/Graph/GraphLiveView.h"

#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Graph/GraphLiveCache.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

namespace Engine {

	namespace Utils {

		// Whether a script slot of `id` in `scene` holds `graph`.
		static bool HoldsGraph(const Scene& scene, UUID id, AssetHandle graph)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			const ScriptComponent* component = entity.IsValid() ? entity.TryGetComponent<ScriptComponent>() : nullptr;
			return component != nullptr && std::ranges::any_of(component->Scripts, [graph](const ScriptSlot& slot)
			{
				return slot.Script.GetHandle() == graph;
			});
		}

		// The play session whose behaviours a view reads, or null: none, or a simulation (it runs no behaviours). A test run's
		// session is read too: graph.liveValues is among the reads a test run admits (EditorContext::CheckTestRunAdmission).
		static const PlaySession* FindLiveSession(const EditorContext& editor)
		{
			const PlaySession* session = editor.GetPlay().GetSession();
			if (session == nullptr || session->GetScripts() == nullptr)
				return nullptr;
			return session;
		}

	}

	std::optional<GraphLiveTarget> FindGraphLiveTarget(const EditorContext& editor, AssetHandle graph, std::string_view behaviour)
	{
		const PlaySession* session = Utils::FindLiveSession(editor);
		if (session == nullptr || !graph.IsValid())
			return std::nullopt;
		const Scene& scene = session->GetScene();
		const std::span<const UUID> selection = editor.GetSelection();
		if (editor.GetSelectionTarget() == SceneTarget::Play && selection.size() == 1 && Utils::HoldsGraph(scene, selection.front(), graph))
			return GraphLiveTarget{ .Entity = selection.front(), .Script = std::string(behaviour) };
		for (const UUID id : scene.GetCanonicalOrder())
		{
			if (Utils::HoldsGraph(scene, id, graph))
				return GraphLiveTarget{ .Entity = id, .Script = std::string(behaviour) };
		}
		return std::nullopt;
	}

	Result<std::string> ReadGraphBehaviourName(EditorContext& editor, AssetHandle graph)
	{
		const AssetRecord* record = graph.IsValid() ? editor.GetAssets().GetRegistry().Find(graph) : nullptr;
		if (record == nullptr)
			return MakeError(ErrorCode::NotFound, "no project asset has the handle {}", graph.ToString());
		ENGINE_TRY_ASSIGN(const std::string text, editor.GetVfs().ReadText(record->SourcePath));
		// graph.liveValues' key (GraphMethods.cpp's LoadLiveGraph), so both share one kept read of the graph.
		GraphLiveCacheKey key{ .Graph = graph, .Path = std::string(record->SourcePath.GetPath()), .SourceHash = XXH64(text), .Version = editor.GetAssets().GetVersion(graph) };
		GraphLiveCache& cache = editor.GetGraphLiveCache();
		if (const GraphLiveCacheEntry* kept = cache.Find(key))
			return kept->Document.Name;
		GraphLoadReport report;
		Result<GraphDocument> document = GraphFromText(text, editor.GetTypeRegistry(), report, true);
		if (!document)
			return MakeError(ErrorCode::ImportFailed, "'{}' cannot be read as a graph: {}", key.Path, document.error().GetMessageText());
		return cache.Insert(std::move(key), std::move(*document)).Document.Name;
	}

	std::string FormatGraphLiveValue(std::string_view type, const VariantValue& value)
	{
		const JsonReader reader(value.Get());
		switch (reader.GetType())
		{
			case JsonType::Bool:
				return reader.ReadBool().value_or(false) ? "true" : "false";
			case JsonType::Integer:
			case JsonType::Float:
				return std::format("{}", reader.ReadDouble().value_or(0.0));
			case JsonType::String:
			{
				const std::string text = reader.ReadString().value_or(std::string());
				if (type != "string")
					return text; // an Entity's or an AssetRef's id
				if (text.size() <= MaxGraphLiveTextBytes)
					return std::format("\"{}\"", text);
				const std::string_view head = std::string_view(text).substr(0, MaxGraphLiveTextBytes);
				return std::format("\"{}...\"", head.substr(0, CompleteUtf8Prefix(head)));
			}
			case JsonType::Array:
			{
				std::string components;
				const size_t count = reader.GetArraySize().value_or(0);
				for (size_t index = 0; index < count; ++index)
				{
					const double component = reader.GetElement(index).and_then([](const JsonReader& element)
					{
						return element.ReadDouble();
					}).value_or(0.0);
					components += std::format("{}{}", index == 0 ? "" : ", ", component);
				}
				return std::format("({})", components);
			}
			case JsonType::Null:
			case JsonType::Object:
				break;
		}
		return std::string(type);
	}

	void GraphLiveView::Update(EditorContext& editor, EditorActions& actions, const std::optional<GraphLiveTarget>& target)
	{
		const PlaySession* session = Utils::FindLiveSession(editor);
		if (!target.has_value() || session == nullptr)
		{
			Reset(actions);
			return;
		}
		if (m_Target != target || m_Session != session->GetSerial())
		{
			Reset(actions);
			m_Target = target;
			m_Session = session->GetSerial();
		}
		if (m_Ticket != 0)
		{
			Result<std::optional<Result<Json>>> taken = actions.TakeResult(m_Ticket);
			if (!taken)
				m_Ticket = 0; // released elsewhere: read again below
			else if (taken->has_value())
			{
				m_Ticket = 0;
				const Result<Json>& read = **taken;
				const StructInfo* type = editor.GetTypeRegistry().FindStruct<GraphLiveValuesResult>();
				GraphLiveValuesResult values;
				Status decoded;
				if (!read)
					decoded = std::unexpected(read.error());
				else if (type == nullptr)
					decoded = MakeError(ErrorCode::InvalidState, "graph.liveValues' result type is not registered");
				else
					decoded = type->FromJson(&values, JsonReader(*read), {});
				if (decoded)
				{
					m_Values = std::move(values);
					m_Error.clear();
				}
				else
				{
					m_Values.reset();
					m_Error = decoded.error().GetMessageText();
				}
			}
		}
		if (m_Ticket != 0)
			return;
		Json params{ { "entity", m_Target->Entity.ToString() } };
		if (!m_Target->Script.empty())
			params["script"] = m_Target->Script;
		Result<uint64_t> submitted = actions.Submit("graph.liveValues", params);
		if (submitted)
			m_Ticket = *submitted;
		else
			m_Error = submitted.error().GetMessageText();
	}

	void GraphLiveView::Reset(EditorActions& actions)
	{
		if (m_Ticket != 0)
		{
			// The read is abandoned: Discard also releases its completion, which nobody will take; a ticket the queue no longer
			// knows needs nothing more.
			static_cast<void>(actions.Discard(m_Ticket));
			m_Ticket = 0;
		}
		Forget();
	}

	void GraphLiveView::Forget()
	{
		m_Target.reset();
		m_Session = 0;
		m_Values.reset();
		m_Error.clear();
	}

	std::span<const GraphLiveValue> GraphLiveView::GetNodeValues(uint32_t node) const
	{
		if (!m_Values.has_value())
			return {};
		const auto [first, last] = std::ranges::equal_range(m_Values->Values, node, {}, &GraphLiveValue::Node);
		return std::span<const GraphLiveValue>(first, last);
	}

	std::vector<GraphLiveError> GraphLiveView::GetNodeErrors(uint32_t node) const
	{
		std::vector<GraphLiveError> errors;
		if (m_Values.has_value())
			std::ranges::copy_if(m_Values->Errors, std::back_inserter(errors), [node](const GraphLiveError& error)
			{
				return error.Node == node;
			});
		return errors;
	}

	std::vector<std::string> GraphLiveView::GetNodeLines(uint32_t node) const
	{
		std::vector<std::string> lines;
		for (const GraphLiveValue& value : GetNodeValues(node))
			lines.push_back(std::format("{} = {}", value.Pin, FormatGraphLiveValue(value.Type, value.Value)));
		for (const GraphLiveError& error : GetNodeErrors(node))
			lines.push_back(error.Count > 1 ? std::format("Error: {} (x{})", error.Message, error.Count) : std::format("Error: {}", error.Message));
		return lines;
	}

	std::string GraphLiveView::FormatMemberLine(const GraphLiveMember& member)
	{
		return std::format("{} = {}", member.Name, FormatGraphLiveValue(member.Type, member.Value));
	}

}
