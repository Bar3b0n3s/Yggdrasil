#include "EditorPCH.h"
#include "Editor/Panels/AutomationPanel.h"

#include "Editor/EditorPanelContext.h"
#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorAutomationControls.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Log.h"

#include <imgui.h>

namespace Engine {

	Status AutomationPanel::Draw(EditorPanelContext& context)
	{
		const auto report = [this](const Error& error)
		{
			const std::string text = error.ToString();
			if (text != m_Error)
				ENGINE_ERROR("Automation: {}", error);
			m_Error = text;
		};
		if (!context.AutomationPreferences.GetState || !context.AutomationPreferences.QueueChange)
			return MakeError(ErrorCode::InvalidState, "automation preference safe-point services are not configured");
		const auto preference = context.AutomationPreferences.GetState();
		if (preference.Failure)
			report(*preference.Failure);
		bool allowed = preference.Allowed;
		ImGui::BeginDisabled(preference.Pending);
		if (ImGui::Checkbox("Allow AI automation", &allowed))
		{
			const Status changed = context.AutomationPreferences.QueueChange(allowed);
			if (!changed)
				report(changed.error());
			else
				m_Error.clear();
		}
		ImGui::EndDisabled();
		if (preference.Pending)
			ImGui::TextUnformatted("Applying automation preference");
		EditorAutomationPolicy policy = context.AutomationControls.GetPolicy();
		bool changed = ImGui::Checkbox("Pause agent requests", &policy.Paused);
		changed = ImGui::Checkbox("Deny agent mutations", &policy.DenyMutations) || changed;
		if (changed)
			context.AutomationControls.SetPolicy(policy);
		ImGui::TextWrapped("Pausing rejects new agent work. Requests already running may finish. Human editor actions remain available.");
		const uint16_t port = context.Automation.GetPort();
		ImGui::TextUnformatted(port == 0 ? "Listener stopped" : std::format("Listening on localhost:{}", port).c_str());
		if (context.Automation.GetSpecification().Listen)
			ImGui::TextWrapped("This launch explicitly enables automation. The saved preference applies to subsequent normal launches.");
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		ImGui::SeparatorText("Connected clients");
		const auto clients = context.Automation.GetClients();
		if (clients.empty())
			ImGui::TextUnformatted("No clients connected");
		for (const AutomationClientInfo& client : clients)
			ImGui::TextUnformatted(std::format("{} {}{}", client.Name, client.Version, client.InProcess ? " (in process)" : "").c_str());
		ImGui::SeparatorText("Recent requests");
		for (const EditorRequestActivity& request : context.AutomationControls.GetRecentRequests())
		{
			ImGui::TextUnformatted(std::format("{} | {} | {} | {} | {:.3f} ms", request.Client, request.RequestId, request.Method,
				request.Completed ? (request.Failed ? "Failed" : "Completed") : "Running", request.DurationMilliseconds)
					.c_str());
		}
		return {};
	}

}
