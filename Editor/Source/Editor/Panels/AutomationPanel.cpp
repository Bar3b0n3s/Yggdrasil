#include "EditorPCH.h"
#include "Editor/Panels/AutomationPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorAutomationControls.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"

#include <imgui.h>

#include <algorithm>

namespace Engine {

	namespace Utils {

		// Copies `text` into a fixed ImGui text buffer, cut at the last whole UTF-8 sequence that fits.
		template<size_t Size>
		static void CopyAutomationText(std::array<char, Size>& buffer, std::string_view text)
		{
			size_t length = std::min(text.size(), Size - 1);
			while (length > 0 && length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
				--length;
			std::copy_n(text.data(), length, buffer.data());
			buffer[length] = '\0';
		}

		static std::string AutomationRequestLabel(std::string_view method)
		{
			std::string label = EditorLabel(method);
			bool wordStart = true;
			for (char& character : label)
			{
				if (character == '.')
					character = ' ';
				if (wordStart && character >= 'a' && character <= 'z')
					character = static_cast<char>(character - 'a' + 'A');
				wordStart = character == ' ';
			}
			return label;
		}

	}

	void AutomationPanel::Report(std::string& channel, const Error& error)
	{
		// Each preference keeps its own last error, so two persistent failures never alternate and log every frame.
		std::string text = error.ToString();
		if (text != channel)
			ENGINE_ERROR("Automation: {}", error);
		channel = std::move(text);
	}

	Status AutomationPanel::Draw(EditorPanelContext& context)
	{
		if (!context.AutomationPreferences.GetState || !context.AutomationPreferences.QueueChange)
			return MakeError(ErrorCode::InvalidState, "automation preference safe-point services are not configured");
		const auto preference = context.AutomationPreferences.GetState();
		if (preference.Failure)
			Report(m_AutomationError, *preference.Failure);
		bool allowed = preference.Allowed;
		ImGui::BeginDisabled(preference.Pending);
		if (ImGui::Checkbox("Allow AI automation", &allowed))
		{
			const Status changed = context.AutomationPreferences.QueueChange(allowed);
			if (!changed)
				Report(m_AutomationError, changed.error());
			else
				m_AutomationError.clear();
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Allow local agents to connect to this editor. This preference is saved for future sessions.");
		if (preference.Pending)
			ImGui::TextUnformatted("Applying automation preference");
		EditorAutomationPolicy policy = context.AutomationControls.GetPolicy();
		bool changed = ImGui::Checkbox("Pause agent requests", &policy.Paused);
		ImGui::SetItemTooltip("Reject new agent work. Already running requests may finish. Human editor actions remain available.");
		changed = ImGui::Checkbox("Deny agent mutations", &policy.DenyMutations) || changed;
		ImGui::SetItemTooltip("Allow agents to inspect the project, take screenshots and run tests while preventing changes to its scene, assets and settings, including discarding unsaved edits.");
		if (changed)
			context.AutomationControls.SetPolicy(policy);
		Utils::EditorSectionHeading("Agent access");
		ImGui::TextWrapped("%s", policy.Paused ? "Paused. New agent work is rejected; running requests may finish." : policy.DenyMutations ? "Read-only access. Agents can inspect the project; changes are blocked."
																																		   : "Agent changes use the editor's commands and appear in History.");
		const uint16_t port = context.Automation.GetPort();
		ImGui::TextDisabled("%s", port == 0 ? "Local connections disabled" : std::format("Listening on localhost:{}", port).c_str());
		if (context.Automation.GetSpecification().Listen)
			ImGui::TextWrapped("This launch explicitly enables automation. The saved preference applies to subsequent normal launches.");
		ENGINE_TRY(DrawExternalEditor(context));
		for (const std::string* error : { &m_AutomationError, &m_ExternalEditorError })
			if (!error->empty())
				ImGui::TextWrapped("%s", error->c_str());
		Utils::EditorSectionHeading("Connected clients");
		const auto clients = context.Automation.GetClients();
		if (clients.empty())
			Utils::EditorEmptyState("No clients connected", "Connected agents appear here with their name and version.");
		for (const AutomationClientInfo& client : clients)
		{
			ImGui::TextWrapped("%s", client.Name.empty() ? "Unnamed client" : client.Name.c_str());
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("%s", std::format("{}{}", client.Version, client.InProcess ? "  /  Editor connection" : "  /  Local connection").c_str());
			ImGui::PopStyleColor();
		}
		Utils::EditorSectionHeading("Recent requests");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##RequestSearch", "Search client or method", m_Search.data(), m_Search.size());
		ImGui::SetItemTooltip("Filter by client or method, ignoring case. Separate alternatives with commas; prefix exclusions with a minus sign.");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::Combo("##RequestState", &m_RequestState, "All requests\0Running\0Completed\0Failed\0");
		ImGui::SetItemTooltip("Show only requests with the selected outcome.");
		const ImGuiTextFilter filter(m_Search.data());
		const auto requests = context.AutomationControls.GetRecentRequests();
		size_t visible = 0;
		if (ImGui::BeginChild("Requests", ImVec2(0.0f, 0.0f)))
		{
			for (size_t index = 0; index < requests.size(); ++index)
			{
				const EditorRequestActivity& request = requests[index];
				if ((m_RequestState == 1 && request.Completed) || (m_RequestState == 2 && (!request.Completed || request.Failed))
					|| (m_RequestState == 3 && !request.Failed) || !filter.PassFilter(std::format("{} {}", request.Client, request.Method).c_str()))
					continue;
				++visible;
				// New activity arrives at the front. Count older duplicates so expanded details stay with their request.
				size_t duplicate = 0;
				for (size_t older = index + 1; older < requests.size(); ++older)
					if (requests[older].Client == request.Client && requests[older].RequestId == request.RequestId && requests[older].Method == request.Method)
						++duplicate;
				ImGui::PushID(static_cast<int>(FNV1a32(std::format("{}/{}/{}/{}", request.Client, request.RequestId, request.Method, duplicate))));
				ImGui::TextColored(request.Failed ? ImVec4(0.95f, 0.48f, 0.48f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
					"%s", request.Completed ? (request.Failed ? "Failed" : "Completed") : "Running");
				ImGui::TextWrapped("%s", Utils::AutomationRequestLabel(request.Method).c_str());
				ImGui::TextWrapped("%s", request.Client.c_str());
				if (request.Completed)
					ImGui::TextDisabled("%.2f ms", request.DurationMilliseconds);
				if (ImGui::TreeNodeEx("Details", ImGuiTreeNodeFlags_SpanAvailWidth))
				{
					ImGui::TextWrapped("Method: %s", request.Method.c_str());
					ImGui::TextWrapped("Request: %s", request.RequestId.c_str());
					ImGui::TreePop();
				}
				ImGui::Separator();
				ImGui::PopID();
			}
			if (visible == 0)
				Utils::EditorEmptyState(requests.empty() ? "No requests yet" : "No matching requests",
					requests.empty() ? "Agent activity and execution times appear here as requests arrive." : "Adjust the search or outcome filter to see more requests.");
		}
		ImGui::EndChild();
		return {};
	}

	std::vector<std::string> AutomationPanel::SplitEditorArguments(std::string_view text)
	{
		std::vector<std::string> arguments;
		while (!text.empty())
		{
			const size_t end = text.find('\n');
			std::string_view line = text.substr(0, end);
			if (line.ends_with('\r'))
				line.remove_suffix(1);
			if (!line.empty())
				arguments.emplace_back(line);
			if (end == std::string_view::npos)
				break;
			text.remove_prefix(end + 1);
		}
		return arguments;
	}

	Status AutomationPanel::DrawExternalEditor(EditorPanelContext& context)
	{
		if (!context.ExternalEditor.GetState || !context.ExternalEditor.QueueChange)
			return MakeError(ErrorCode::InvalidState, "external editor preference safe-point services are not configured");
		const EditorExternalEditorState saved = context.ExternalEditor.GetState();
		if (saved.Failure)
			Report(m_ExternalEditorError, *saved.Failure);
		if (!m_EditorEdited && !saved.Pending)
		{
			std::string arguments;
			for (const std::string& argument : saved.Arguments)
				arguments += (arguments.empty() ? "" : "\n") + argument;
			Utils::CopyAutomationText(m_EditorExecutable, saved.Executable);
			Utils::CopyAutomationText(m_EditorArguments, arguments);
		}
		Utils::EditorSectionHeading("External source editor");
		ImGui::BeginDisabled(saved.Pending);
		ImGui::SetNextItemWidth(-1.0f);
		bool edited = ImGui::InputTextWithHint("##EditorExecutable", "Executable, such as code", m_EditorExecutable.data(), m_EditorExecutable.size());
		ImGui::SetItemTooltip("The program Open source actions start with a script. Leave it and the arguments empty to turn them off.");
		edited = ImGui::InputTextMultiline("##EditorArguments", m_EditorArguments.data(), m_EditorArguments.size(),
					 ImVec2(-1.0f, ImGui::GetTextLineHeight() * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f))
			|| edited;
		ImGui::SetItemTooltip("One argument per line, passed without a shell. {path} becomes the script's path and {line} its line; "
							  "with no arguments the path is the only one.");
		if (edited)
			m_EditorEdited = true;
		ImGui::BeginDisabled(!m_EditorEdited);
		if (ImGui::Button("Save editor"))
		{
			const Status queued = context.ExternalEditor.QueueChange(std::string(m_EditorExecutable.data()), SplitEditorArguments(m_EditorArguments.data()));
			if (!queued)
				Report(m_ExternalEditorError, queued.error());
			else
			{
				m_EditorEdited = false;
				m_ExternalEditorError.clear();
			}
		}
		ImGui::SetItemTooltip("Save the external editor for this and future sessions.");
		ImGui::SameLine();
		if (ImGui::Button("Revert"))
			m_EditorEdited = false;
		ImGui::SetItemTooltip("Discard the edits and show the saved external editor.");
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		if (saved.Pending)
			ImGui::TextUnformatted("Saving the external editor");
		return {};
	}

}
