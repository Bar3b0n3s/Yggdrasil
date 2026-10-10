#include "EditorPCH.h"
#include "Editor/Panels/SceneHierarchyPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Icons.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <set>

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
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##Search", "Search entities", &m_Search);
		EditorContext& editor = context.Editor;
		PlaySession* session = editor.GetPlay().GetSession();
		Scene* scene = session != nullptr ? &session->GetScene() : editor.HasScene() ? &editor.GetScene()
																					 : nullptr;
		if (scene == nullptr)
		{
			Utils::EditorEmptyState("Your scene starts here", "Open or create a scene from the File menu to add entities.");
			return {};
		}
		const char* target = session == nullptr ? "Edit" : "Play";
		if (session != nullptr)
			ImGui::TextWrapped("Preview scene: changes end on Stop");
		if (editor.IsReadOnly())
			ImGui::TextDisabled("Read-only project");
		auto queue = [this, &context](std::string_view method, const Json& params)
		{
			auto submitted = context.Actions.Submit(method, params);
			if (submitted)
				m_Tickets.push_back(*submitted);
			else
				ReportFailure(submitted.error());
		};
		std::set<UUID> matching;
		std::function<bool(UUID)> collectMatches = [this, scene, &matching, &collectMatches](UUID id)
		{
			const ConstEntity entity = scene->FindEntityByID(id);
			const auto& name = entity.GetName();
			bool matched = m_Search.empty() || std::search(name.begin(), name.end(), m_Search.begin(), m_Search.end(), [](unsigned char left, unsigned char right)
			{
				return std::tolower(left) == std::tolower(right);
			}) != name.end();
			for (const UUID child : entity.GetChildren())
				matched = collectMatches(child) || matched;
			if (matched)
				matching.insert(id);
			return matched;
		};
		for (const UUID root : scene->GetRootEntities())
			collectMatches(root);
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
		std::function<void(UUID, uint32_t)> draw = [this, &context, &queue, &drop, &matching, &draw, scene, target](UUID id, uint32_t sibling)
		{
			if (!matching.contains(id))
				return;
			ConstEntity entity = scene->FindEntityByID(id);
			ImGui::PushID(id.ToString().c_str());
			ImGui::InvisibleButton("##Insert", ImVec2(ImGui::GetContentRegionAvail().x, 3.0f));
			drop(entity.GetParent().IsValid() ? entity.GetParent().GetUUID() : UUID{}, sibling);
			const auto selection = context.Editor.GetSelection();
			const bool selected = std::find(selection.begin(), selection.end(), id) != selection.end();
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap;
			if (entity.GetChildren().empty())
				flags |= ImGuiTreeNodeFlags_Leaf;
			if (selected)
				flags |= ImGuiTreeNodeFlags_Selected;
			if (!m_Search.empty())
				ImGui::SetNextItemOpen(true);
			const ImVec2 row = ImGui::GetCursorScreenPos();
			const float right = row.x + ImGui::GetContentRegionAvail().x;
			const float activeWidth = ImGui::GetTextLineHeight();
			const float toggleStart = right - activeWidth;
			const bool expanded = ImGui::TreeNodeEx("##Entity", flags, "%s", "");
			const ImVec2 rowEnd = ImGui::GetItemRectMax();
			const bool prefab = entity.HasComponent<PrefabInstanceComponent>() || entity.HasComponent<PrefabLinkComponent>();
			const float nameStart = row.x + ImGui::GetTreeNodeToLabelSpacing();
			const float iconWidth = prefab ? ImGui::GetTextLineHeightWithSpacing() : 0.0f;
			if (!entity.IsActive())
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			if (prefab)
				DrawEditorIcon(*ImGui::GetWindowDrawList(), EditorIcon::Prefab, { nameStart, row.y }, ImGui::GetTextLineHeight(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
			ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(nameStart + iconWidth, row.y),
				ImVec2(std::max(nameStart + iconWidth, toggleStart - ImGui::GetStyle().ItemSpacing.x), rowEnd.y), toggleStart - ImGui::GetStyle().ItemSpacing.x,
				entity.GetName().data(), entity.GetName().data() + entity.GetName().size(), nullptr);
			if (!entity.IsActive())
				ImGui::PopStyleColor();
			if (ImGui::IsItemHovered() && ImGui::GetMousePos().x < toggleStart)
				ImGui::SetTooltip("%s%s%s", entity.GetName().c_str(), prefab ? "\nPrefab instance" : "", entity.IsActive() ? "" : "\nInactive in this scene");
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && ImGui::GetMousePos().x < toggleStart)
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
			if (!context.Editor.IsReadOnly() && ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload("ENGINE_ENTITY", &id, sizeof(id));
				ImGui::TextUnformatted(entity.GetName().c_str());
				ImGui::EndDragDropSource();
			}
			drop(id, std::nullopt);
			if (ImGui::BeginPopupContextItem())
			{
				ImGui::TextUnformatted(entity.GetName().c_str());
				ImGui::Separator();
				ImGui::BeginDisabled(context.Editor.IsReadOnly());
				if (ImGui::MenuItem("Rename..."))
				{
					m_RenameEntity = id;
					m_Rename = entity.GetName();
					m_RenameRevision = context.Editor.GetRevision();
					const auto* renameSession = context.Editor.GetPlay().GetSession();
					m_RenamePlay = renameSession != nullptr;
					m_RenameSessionSerial = renameSession != nullptr ? renameSession->GetSerial() : 0;
					m_RenameSceneGeneration = renameSession != nullptr ? renameSession->GetSceneGeneration() : 0;
					m_ShowRename = true;
				}
				if (ImGui::MenuItem("Duplicate"))
					queue("entity.duplicate", Json{ { "entities", Json::array({ id.ToString() }) }, { "target", target } });
				if (ImGui::MenuItem("Create child"))
					queue("entity.create", Json{ { "name", "Entity" }, { "parent", id.ToString() }, { "target", target } });
				ImGui::Separator();
				bool active = entity.IsActiveSelf();
				if (ImGui::Checkbox("Active", &active))
					queue("entity.update", Json{ { "entity", id.ToString() }, { "active", active }, { "target", target } });
				if (ImGui::MenuItem("Delete"))
					queue("entity.destroy", Json{ { "entities", Json::array({ id.ToString() }) }, { "target", target } });
				ImGui::EndDisabled();
				ImGui::EndPopup();
			}
			const ImVec2 next = ImGui::GetCursorScreenPos();
			ImGui::SetCursorScreenPos(ImVec2(toggleStart, row.y));
			ImGui::BeginDisabled(context.Editor.IsReadOnly());
			if (ImGui::InvisibleButton("##Active", ImVec2(activeWidth, ImGui::GetTextLineHeight())))
				queue("entity.update", Json{ { "entity", id.ToString() }, { "active", !entity.IsActiveSelf() }, { "target", target } });
			const ImVec2 center(toggleStart + activeWidth * 0.5f, row.y + ImGui::GetTextLineHeight() * 0.5f);
			const ImU32 color = ImGui::GetColorU32(entity.IsActive() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			ImGui::GetWindowDrawList()->AddCircle(center, activeWidth * 0.25f, color, 12, 1.0f);
			if (entity.IsActiveSelf())
				ImGui::GetWindowDrawList()->AddCircleFilled(center, activeWidth * 0.13f, color, 12);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", context.Editor.IsReadOnly() ? "Read-only project" : !entity.IsActiveSelf() ? "Enable entity in the scene"
						: !entity.IsActive()                                                                       ? "Disabled by a parent. Click to disable this entity too."
																												   : "Disable entity in the scene (rendering and simulation)");
			ImGui::EndDisabled();
			ImGui::SetCursorScreenPos(next);
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
		const std::string count = std::format("{}", scene->GetEntityCount());
		const float countWidth = ImGui::CalcTextSize(count.c_str()).x;
		if (ImGui::GetContentRegionAvail().x > countWidth + ImGui::GetItemRectSize().x + ImGui::GetStyle().ItemSpacing.x)
		{
			ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - countWidth);
			ImGui::TextDisabled("%s", count.c_str());
		}
		if (m_ShowRename)
		{
			ImGui::OpenPopup("Rename entity");
			m_ShowRename = false;
		}
		ImGui::SetNextWindowSize(ImVec2(340.0f * Utils::EditorUiScale(), 0), ImGuiCond_Appearing);
		if (ImGui::BeginPopupModal("Rename entity", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			// Runtime copies can reuse every entity UUID without changing the edit revision.
			const bool sameSession = session == nullptr || (m_RenameSessionSerial == session->GetSerial() && m_RenameSceneGeneration == session->GetSceneGeneration());
			const bool current = m_RenameRevision == editor.GetRevision() && m_RenamePlay == (session != nullptr) && sameSession && scene->FindEntityByID(m_RenameEntity).IsValid();
			ImGui::TextUnformatted("Entity name");
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::IsWindowAppearing())
				ImGui::SetKeyboardFocusHere();
			const bool enter = ImGui::InputText("##Name", &m_Rename, ImGuiInputTextFlags_EnterReturnsTrue);
			if (!current)
				ImGui::TextWrapped("The scene changed. Cancel and select the entity again to rename it.");
			const bool canRename = current && !editor.IsReadOnly() && !m_Rename.empty();
			ImGui::BeginDisabled(!canRename);
			if (ImGui::Button("Rename") || (enter && canRename))
			{
				queue("entity.update", Json{ { "entity", m_RenameEntity.ToString() }, { "name", m_Rename }, { "target", target } });
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		uint32_t index = 0;
		for (const UUID root : scene->GetRootEntities())
			draw(root, index++);
		if (scene->GetEntityCount() == 0)
			Utils::EditorEmptyState("An empty scene", "Create your first entity above. Right-click an entity for more actions.");
		else if (matching.empty())
			ImGui::TextDisabled("No matching entities");
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::Selectable("Drop here to make a root", false);
		ImGui::PopStyleColor();
		drop({}, std::nullopt);
		return {};
	}

}
