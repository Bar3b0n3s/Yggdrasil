#include "EditorPCH.h"
#include "Editor/Panels/UndoHistoryPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Log.h"

#include <imgui.h>

namespace Engine {

	Status UndoHistoryPanel::Draw(EditorPanelContext& context)
	{
		const auto report = [this](const Error& error)
		{
			m_Error = error.ToString();
			ENGINE_ERROR("Undo history: {}", error);
		};
		if (m_Ticket != 0)
		{
			auto result = context.Actions.TakeResult(m_Ticket);
			if (!result || result->has_value())
			{
				m_Ticket = 0;
				if (!result)
					report(result.error());
				else if (!**result)
					report((**result).error());
			}
		}
		const CommandHistory& history = context.Editor.GetHistory();
		const auto submit = [this, &context, &report](std::string_view method)
		{
			auto ticket = context.Actions.Submit(method, Json{ { "steps", 1 }, { "ifRevision", context.Editor.GetRevision() } });
			if (ticket)
			{
				m_Ticket = *ticket;
				m_Error.clear();
			}
			else
				report(ticket.error());
		};
		ImGui::BeginDisabled(m_Ticket != 0 || !history.CanUndo() || context.Editor.IsReadOnly());
		if (Utils::EditorToolbarButton("Undo", history.CanUndo() ? history.GetUndoLabel().c_str() : "There are no applied changes to undo."))
			submit("edit.undo");
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(m_Ticket != 0 || !history.CanRedo() || context.Editor.IsReadOnly());
		if (Utils::EditorToolbarButton("Redo", history.CanRedo() ? history.GetRedoLabel().c_str() : "There are no undone changes to restore."))
			submit("edit.redo");
		ImGui::EndDisabled();
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		if (context.Editor.HasScene())
			ImGui::TextWrapped("%s", context.Editor.IsSceneDirty() ? "Unsaved scene changes" : "Scene matches the saved state");
		ImGui::TextDisabled("%zu applied  /  %zu to redo", history.GetUndoCount(), history.GetRedoCount());
		ImGui::SetItemTooltip("History uses %.1f KiB of its %.0f MiB limit.", static_cast<double>(history.GetMemorySize()) / 1024.0,
			static_cast<double>(history.GetLimits().MaxBytes) / (1024.0 * 1024.0));
		if (context.Editor.IsReadOnly())
			ImGui::TextWrapped("Read-only project. Undo and redo are unavailable.");
		if (m_Ticket != 0)
			ImGui::TextDisabled("Applying change...");
		const auto entries = history.GetEntries(history.GetLimits().MaxEntries);
		if (entries.empty())
		{
			Utils::EditorEmptyState("No changes yet", "Project and scene edits appear here. Changes made by agents carry an [agent] label.");
			return {};
		}
		if (ImGui::BeginChild("HistoryEntries", ImVec2(0.0f, 0.0f)))
		{
			for (size_t index = 0; index < entries.size(); ++index)
			{
				if (index == history.GetUndoCount())
					Utils::EditorSectionHeading("Current position");
				const CommandHistoryEntry& entry = entries[index];
				const bool applied = index < history.GetUndoCount();
				ImGui::PushID(std::to_string(entry.Sequence).c_str());
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(applied ? ImGuiCol_Text : ImGuiCol_TextDisabled));
				ImGui::TextWrapped("%s", entry.Label.c_str());
				ImGui::PopStyleColor();
				ImGui::SetItemTooltip("%s", std::format("{}; revision {} -> {}; {} bytes", entry.Origin == CommandOrigin::Agent ? "Agent" : "User", entry.RevisionBefore, entry.RevisionAfter, entry.MemorySize).c_str());
				ImGui::TextDisabled("%s / %s", entry.Origin == CommandOrigin::Agent ? "Agent" : "You", applied ? "Applied" : "Undone");
				ImGui::Separator();
				ImGui::PopID();
			}
			if (history.GetUndoCount() == entries.size())
				Utils::EditorSectionHeading("Current position");
		}
		ImGui::EndChild();
		return {};
	}

}
