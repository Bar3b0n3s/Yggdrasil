#include "EditorPCH.h"
#include "Editor/Panels/ConsolePanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
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
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const bool wide = ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 25.0f;
		const float filterWidth = wide ? (ImGui::GetContentRegionAvail().x - spacing) * 0.5f : ImGui::GetContentRegionAvail().x;
		ImGui::SetNextItemWidth(filterWidth);
		ImGui::Combo("##MinimumLevel", &m_MinimumLevel, "All levels\0Info and above\0Warnings and above\0Errors and above\0Critical only\0");
		ImGui::SetItemTooltip("Show messages at or above this severity.");
		if (wide)
			ImGui::SameLine();
		ImGui::SetNextItemWidth(filterWidth);
		ImGui::Combo("##Logger", &m_Logger, "All sources\0Engine\0Application\0Scripts\0");
		ImGui::SetItemTooltip("Filter messages by the system that produced them.");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##Search", "Search messages", m_Search.data(), m_Search.size());
		ImGui::SetItemTooltip("Find text in log messages. Search is case-sensitive.");
		ImGui::Checkbox("Follow newest", &m_Follow);
		ImGui::SetItemTooltip("Follow incoming messages while at the bottom. Scroll up to read earlier entries.");
		if (ImGui::GetContentRegionAvail().x >= ImGui::CalcTextSize("Follow newestClear view").x + ImGui::GetFrameHeight() + spacing * 2.0f + ImGui::GetStyle().FramePadding.x * 2.0f)
			ImGui::SameLine();
		RingBufferSink& ring = Log::GetRingBuffer();
		if (Utils::EditorToolbarButton("Clear view", "Hide current messages in this panel. The shared log remains available to automation."))
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
		ImGui::TextDisabled("%zu messages", logs.Entries.size());
		if (logs.DroppedCount != 0)
			ImGui::TextUnformatted(std::format("{} older entries no longer available", logs.DroppedCount).c_str());
		if (ImGui::BeginChild("ConsoleEntries", ImVec2(0.0f, 0.0f)))
		{
			const bool wasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeight();
			if (logs.Entries.empty())
				Utils::EditorEmptyState("No messages to show", "Messages from the engine, application and scripts appear here. Adjust the filters to see more.");
			for (const LogEntry& entry : logs.Entries)
			{
				ImGui::PushID(std::to_string(entry.Seq).c_str());
				const ImVec4 color = entry.Level >= LogLevel::Error ? ImVec4(0.95f, 0.48f, 0.48f, 1.0f)
					: entry.Level == LogLevel::Warn                 ? ImVec4(0.91f, 0.73f, 0.39f, 1.0f)
																	: ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
				ImGui::TextColored(color, "%s", std::string(LogLevelToString(entry.Level)).c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("%s", std::string(LogChannelToString(entry.Logger)).c_str());
				ImGui::TextWrapped("%s", entry.Message.c_str());
				const std::string& file = entry.ScriptFile.empty() ? entry.File : entry.ScriptFile;
				const uint32_t line = entry.ScriptFile.empty() ? entry.Line : entry.ScriptLine;
				if (!file.empty())
				{
					const size_t separator = file.find_last_of("/\\");
					const std::string name = file.substr(separator == std::string::npos ? 0 : separator + 1);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					ImGui::TextWrapped("%s", std::format("{}:{}", name, line).c_str());
					ImGui::PopStyleColor();
					ImGui::SetItemTooltip("%s", file.c_str());
					if (ImGui::SmallButton("Open source"))
					{
						const Status opened = context.OpenSource ? context.OpenSource(FileSystem::PathFromUtf8(file), line)
																 : Status(MakeError(ErrorCode::InvalidState, "source editor is not configured"));
						if (!opened)
							report(opened.error());
					}
					ImGui::SetItemTooltip("Open %s at line %u.", file.c_str(), line);
				}
				if (entry.EntityId.IsValid())
				{
					ImGui::BeginDisabled(m_SelectionTicket != 0 || !context.Editor.HasScene());
					if (ImGui::SmallButton("Select entity"))
					{
						auto ticket = context.Actions.Submit("edit.select", Json{ { "entities", Json::array({ entry.EntityId.ToString() }) }, { "ifRevision", context.Editor.GetRevision() } });
						if (ticket)
							m_SelectionTicket = *ticket;
						else
							report(ticket.error());
					}
					ImGui::EndDisabled();
					ImGui::SetItemTooltip("Select the entity associated with this message: %s", entry.EntityId.ToString().c_str());
				}
				ImGui::Separator();
				ImGui::PopID();
			}
			if (m_Follow && wasAtBottom)
				ImGui::SetScrollHereY(1.0f);
		}
		ImGui::EndChild();
		return {};
	}

}
