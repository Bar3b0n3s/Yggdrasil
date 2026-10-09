#include "EditorPCH.h"
#include "Editor/Panels/UndoHistoryPanel.h"

#include "Editor/EditorPanelContext.h"
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
		if (ImGui::Button("Undo"))
			submit("edit.undo");
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(m_Ticket != 0 || !history.CanRedo() || context.Editor.IsReadOnly());
		if (ImGui::Button("Redo"))
			submit("edit.redo");
		ImGui::EndDisabled();
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		ImGui::TextUnformatted(std::format("{} applied, {} redo; {} bytes", history.GetUndoCount(), history.GetRedoCount(), history.GetMemorySize()).c_str());
		const auto entries = history.GetEntries(history.GetLimits().MaxEntries);
		for (size_t index = 0; index < entries.size(); ++index)
		{
			if (index == history.GetUndoCount())
				ImGui::SeparatorText("Current position");
			const CommandHistoryEntry& entry = entries[index];
			ImGui::TextUnformatted(std::format("{} {}", entry.Sequence, entry.Label).c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", std::format("{}; revision {} -> {}; {} bytes", entry.Origin == CommandOrigin::Agent ? "Agent" : "User", entry.RevisionBefore, entry.RevisionAfter, entry.MemorySize).c_str());
		}
		if (history.GetUndoCount() == entries.size())
			ImGui::SeparatorText("Current position");
		return {};
	}

}
