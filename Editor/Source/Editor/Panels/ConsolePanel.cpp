#include "EditorPCH.h"
#include "Editor/Panels/ConsolePanel.h"

#include "Editor/EditorPanelContext.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"

#include <imgui.h>

namespace Engine {

	Status ConsolePanel::Draw(EditorPanelContext& context)
	{
		const auto report = [this](const Error& error)
		{
			m_Error = error.ToString();
			ENGINE_ERROR("Console: {}", error);
		};
		if (m_SelectionTicket != 0)
		{
			auto result = context.Actions.TakeResult(m_SelectionTicket);
			if (!result || result->has_value())
			{
				m_SelectionTicket = 0;
				if (!result)
					report(result.error());
				else if (!**result)
					report((**result).error());
			}
		}
		ImGui::Combo("Minimum level", &m_MinimumLevel, "Trace\0Info\0Warn\0Error\0Critical\0");
		ImGui::Combo("Logger", &m_Logger, "All\0Engine\0App\0Script\0");
		ImGui::InputText("Search", m_Search.data(), m_Search.size());
		ImGui::Checkbox("Follow newest", &m_Follow);
		ImGui::SameLine();
		RingBufferSink& ring = Log::GetRingBuffer();
		if (ImGui::Button("Clear view"))
			m_ClearBefore = ring.GetNextSeq();
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		LogQuery query;
		query.Cursor = m_ClearBefore;
		query.MinimumLevel = static_cast<LogLevel>(m_MinimumLevel);
		query.Contains = m_Search.data();
		query.Limit = ring.GetCapacity();
		if (m_Logger != 0)
			query.Channels.push_back(static_cast<LogChannel>(m_Logger - 1));
		const LogReadResult logs = ring.Read(query);
		if (logs.DroppedCount != 0)
			ImGui::TextUnformatted(std::format("{} older entries no longer available", logs.DroppedCount).c_str());
		if (ImGui::BeginChild("ConsoleEntries", ImVec2(0.0f, 0.0f)))
		{
			for (const LogEntry& entry : logs.Entries)
			{
				ImGui::PushID(std::to_string(entry.Seq).c_str());
				ImGui::TextUnformatted(std::format("[{}] [{}] {}", LogLevelToString(entry.Level), LogChannelToString(entry.Logger), entry.Message).c_str());
				const std::string& file = entry.ScriptFile.empty() ? entry.File : entry.ScriptFile;
				const uint32_t line = entry.ScriptFile.empty() ? entry.Line : entry.ScriptLine;
				if (!file.empty())
				{
					if (ImGui::SmallButton(std::format("{}:{}###Source", file, line).c_str()))
					{
						const Status opened = context.OpenSource ? context.OpenSource(FileSystem::PathFromUtf8(file), line)
																 : Status(MakeError(ErrorCode::InvalidState, "source editor is not configured"));
						if (!opened)
							report(opened.error());
					}
				}
				if (entry.EntityId.IsValid())
				{
					ImGui::BeginDisabled(m_SelectionTicket != 0 || !context.Editor.HasScene());
					if (ImGui::SmallButton(std::format("Select {}", entry.EntityId).c_str()))
					{
						auto ticket = context.Actions.Submit("edit.select", Json{ { "entities", Json::array({ entry.EntityId.ToString() }) }, { "ifRevision", context.Editor.GetRevision() } });
						if (ticket)
							m_SelectionTicket = *ticket;
						else
							report(ticket.error());
					}
					ImGui::EndDisabled();
				}
				ImGui::PopID();
			}
			if (m_Follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
				ImGui::SetScrollHereY(1.0f);
		}
		ImGui::EndChild();
		return {};
	}

}
