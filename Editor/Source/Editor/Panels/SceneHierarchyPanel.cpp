#include "EditorPCH.h"
#include "Editor/Panels/SceneHierarchyPanel.h"

#include "Editor/EditorPanelContext.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstring>
#include <functional>

namespace Engine {

	void SceneHierarchyPanel::ReportFailure(const Error& error)
	{
		const std::string message = error.GetMessageText();
		if (m_Error != message)
			ENGINE_ERROR("SceneHierarchyPanel: {}", error.ToString());
		m_Error = message;
	}

	Status SceneHierarchyPanel::Draw(EditorPanelContext& context)
	{
		for (auto ticket = m_Tickets.begin(); ticket != m_Tickets.end();)
		{
			auto result = context.Actions.TakeResult(*ticket);
			if (!result || result->has_value())
			{
				if (!result)
					ReportFailure(result.error());
				else if (!**result)
					ReportFailure((**result).error());
				ticket = m_Tickets.erase(ticket);
			}
			else
				++ticket;
		}
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		ImGui::InputTextWithHint("##Search", "Search entities", &m_Search);
		EditorContext& editor = context.Editor;
		PlaySession* session = editor.GetPlay().GetSession();
		Scene* scene = session != nullptr ? &session->GetScene() : editor.HasScene() ? &editor.GetScene()
																					 : nullptr;
		if (scene == nullptr)
		{
			ImGui::TextDisabled("Open a scene to view its hierarchy.");
			return {};
		}
		const char* target = session == nullptr ? "Edit" : "Play";
		if (session != nullptr)
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Runtime scene - changes end on Stop");
		auto queue = [this, &context](std::string_view method, const Json& params)
		{
			auto submitted = context.Actions.Submit(method, params);
			if (submitted)
				m_Tickets.push_back(*submitted);
			else
				ReportFailure(submitted.error());
		};
		std::function<bool(UUID)> matches = [this, scene, &matches](UUID id)
		{
			const ConstEntity entity = scene->FindEntityByID(id);
			if (m_Search.empty() || entity.GetName().contains(m_Search))
				return true;
			for (const UUID child : entity.GetChildren())
			{
				if (matches(child))
					return true;
			}
			return false;
		};
		auto drop = [&queue, &editor, scene, target](UUID parent, std::optional<uint32_t> index)
		{
			if (editor.IsReadOnly() || !ImGui::BeginDragDropTarget())
				return;
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENGINE_ENTITY"))
			{
				if (payload->DataSize == sizeof(UUID))
				{
					UUID child;
					std::memcpy(&child, payload->Data, sizeof(child));
					bool cycle = false;
					for (ConstEntity ancestor = scene->FindEntityByID(parent); ancestor.IsValid(); ancestor = ancestor.GetParent())
						cycle |= ancestor.GetUUID() == child;
					if (!cycle && scene->FindEntityByID(child).IsValid())
					{
						Json params{ { "entity", child.ToString() }, { "parent", parent.IsValid() ? parent.ToString() : "" }, { "keepWorld", true }, { "target", target } };
						if (index)
							params["index"] = *index;
						queue("entity.reparent", params);
					}
				}
			}
			ImGui::EndDragDropTarget();
		};
		std::function<void(UUID, uint32_t)> draw = [this, &context, &queue, &drop, &matches, &draw, scene, target](UUID id, uint32_t sibling)
		{
			if (!matches(id))
				return;
			ConstEntity entity = scene->FindEntityByID(id);
			ImGui::PushID(id.ToString().c_str());
			ImGui::InvisibleButton("##Insert", ImVec2(ImGui::GetContentRegionAvail().x, 3.0f));
			drop(entity.GetParent().IsValid() ? entity.GetParent().GetUUID() : UUID{}, sibling);
			const auto selection = context.Editor.GetSelection();
			const bool selected = std::find(selection.begin(), selection.end(), id) != selection.end();
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (entity.GetChildren().empty())
				flags |= ImGuiTreeNodeFlags_Leaf;
			if (selected)
				flags |= ImGuiTreeNodeFlags_Selected;
			if (!m_Search.empty())
				ImGui::SetNextItemOpen(true);
			const bool expanded = ImGui::TreeNodeEx("##Entity", flags, "%s%s", entity.GetName().c_str(),
				entity.HasComponent<PrefabInstanceComponent>() || entity.HasComponent<PrefabLinkComponent>() ? " [Prefab]" : "");
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
			{
				Json entities = Json::array();
				if (ImGui::GetIO().KeyCtrl)
				{
					for (const UUID current : selection)
					{
						if (current != id)
							entities.push_back(current.ToString());
					}
				}
				if (!selected || !ImGui::GetIO().KeyCtrl)
					entities.push_back(id.ToString());
				queue("edit.select", Json{ { "entities", std::move(entities) } });
			}
			if (ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload("ENGINE_ENTITY", &id, sizeof(id));
				ImGui::TextUnformatted(entity.GetName().c_str());
				ImGui::EndDragDropSource();
			}
			drop(id, std::nullopt);
			if (ImGui::BeginPopupContextItem())
			{
				ImGui::BeginDisabled(context.Editor.IsReadOnly());
				bool active = entity.IsActiveSelf();
				if (ImGui::Checkbox("Active", &active))
					queue("entity.update", Json{ { "entity", id.ToString() }, { "active", active }, { "target", target } });
				if (ImGui::MenuItem("Delete"))
					queue("entity.destroy", Json{ { "entities", Json::array({ id.ToString() }) }, { "target", target } });
				ImGui::EndDisabled();
				ImGui::EndPopup();
			}
			if (expanded)
			{
				uint32_t index = 0;
				for (const UUID child : entity.GetChildren())
					draw(child, index++);
				ImGui::TreePop();
			}
			ImGui::PopID();
		};
		ImGui::BeginDisabled(editor.IsReadOnly());
		if (ImGui::Button("Create entity"))
			queue("entity.create", Json{ { "name", "Entity" }, { "target", target } });
		ImGui::EndDisabled();
		uint32_t index = 0;
		for (const UUID root : scene->GetRootEntities())
			draw(root, index++);
		ImGui::Selectable("Drop here to make a root", false);
		drop({}, std::nullopt);
		return {};
	}

}
