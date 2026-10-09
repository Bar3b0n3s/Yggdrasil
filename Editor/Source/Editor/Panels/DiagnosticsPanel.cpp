#include "EditorPCH.h"
#include "Editor/Panels/DiagnosticsPanel.h"

#include "Editor/EditorPanelContext.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <imgui.h>

namespace Engine {

	Status DiagnosticsPanel::Draw(EditorPanelContext& context)
	{
		const auto report = [this](const Error& error)
		{
			m_Error = error.ToString();
			ENGINE_ERROR("Diagnostics: {}", error);
		};
		const auto project = context.Editor.HasProject() ? context.Editor.GetProject().GetProjectFile() : std::filesystem::path{};
		if (project != m_Project)
		{
			for (uint64_t* ticket : { &m_Ticket, &m_SelectionTicket })
			{
				if (*ticket != 0)
				{
					const Status cancelled = context.Actions.Cancel(*ticket);
					if (!cancelled)
						report(cancelled.error());
					*ticket = 0;
				}
			}
			m_Project = project;
			m_HasReport = false;
			m_Selected.clear();
			m_Refresh = true;
			m_Fixing = false;
		}
		ImGui::TextUnformatted("Script type checking unavailable until M13");
		if (project.empty())
		{
			ImGui::TextUnformatted("Open a project to view diagnostics");
			return {};
		}
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
				else
				{
					const StructInfo* type = context.Editor.GetTypeRegistry().FindStruct<ProjectValidateResult>();
					if (!type)
						return MakeError(ErrorCode::InvalidState, "project validation result type is not registered");
					ProjectValidateResult diagnostics;
					const Status decoded = type->FromJson(&diagnostics, JsonReader(***result), {});
					if (!decoded)
						report(decoded.error());
					else
					{
						m_Report = std::move(diagnostics);
						m_HasReport = true;
						m_Selected.clear();
						m_Error.clear();
						m_Refresh = m_Fixing;
					}
				}
				m_Fixing = false;
			}
		}
		ImGui::BeginDisabled(m_Ticket != 0);
		if (ImGui::Button("Validate project"))
			m_Refresh = true;
		ImGui::EndDisabled();
		if (m_Refresh && m_Ticket == 0)
		{
			m_Refresh = false;
			m_ReportRevision = context.Editor.GetRevision();
			auto ticket = context.Actions.Submit("project.validate", Json{ { "scope", "Project" }, { "ifRevision", m_ReportRevision } });
			if (ticket)
				m_Ticket = *ticket;
			else
				report(ticket.error());
		}
		ImGui::Combo("Severity", &m_Severity, "All\0Errors\0Warnings\0");
		ImGui::InputText("Search diagnostics", m_Search.data(), m_Search.size());
		ImGui::BeginDisabled(m_Ticket != 0 || m_Selected.empty() || context.Editor.IsReadOnly() || m_ReportRevision != context.Editor.GetRevision());
		if (ImGui::Button("Fix selected"))
		{
			auto ticket = context.Actions.Submit("project.validate", Json{ { "scope", "Project" }, { "fix", m_Selected }, { "ifRevision", m_ReportRevision } });
			if (ticket)
			{
				m_Ticket = *ticket;
				// Refresh only after success so a failed fix remains visible until the user retries.
				m_Fixing = true;
			}
			else
				report(ticket.error());
		}
		ImGui::EndDisabled();
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		if (!m_HasReport)
		{
			ImGui::TextUnformatted(m_Ticket != 0 ? "Validation queued" : "No validation report available");
			return {};
		}
		ImGui::TextUnformatted(std::format("{} errors, {} warnings", m_Report.ErrorCount, m_Report.WarningCount).c_str());
		if (m_ReportRevision != context.Editor.GetRevision())
			ImGui::TextUnformatted("Project changed; refresh diagnostics before applying fixes");
		for (const ProjectDiagnostic& diagnostic : m_Report.Diagnostics)
		{
			const bool isError = diagnostic.Severity == DiagnosticSeverity::Error;
			if ((m_Severity == 1 && !isError) || (m_Severity == 2 && isError))
				continue;
			const std::string text = std::format("{} | {} | {} | {}", diagnostic.Id, diagnostic.Code, diagnostic.Message, diagnostic.File);
			if (m_Search[0] != '\0' && !text.contains(m_Search.data()))
				continue;
			ImGui::PushID(diagnostic.Id.c_str());
			if (diagnostic.AutoFixable)
			{
				bool selected = m_Selected.contains(diagnostic.Id);
				if (ImGui::Checkbox("##Fix", &selected))
				{
					if (selected)
						m_Selected.insert(diagnostic.Id);
					else
						m_Selected.erase(diagnostic.Id);
				}
				ImGui::SameLine();
			}
			ImGui::TextWrapped("%s: %s", isError ? "Error" : "Warning", text.c_str());
			if (!diagnostic.Hint.empty())
				ImGui::TextWrapped("%s", diagnostic.Hint.c_str());
			if (!diagnostic.File.empty() && ImGui::SmallButton("Open source"))
			{
				const auto path = context.Editor.GetProject().GetRoot() / FileSystem::PathFromUtf8(diagnostic.File);
				const Status opened = context.OpenSource ? context.OpenSource(path, diagnostic.Line)
														 : Status(MakeError(ErrorCode::InvalidState, "source editor is not configured"));
				if (!opened)
					report(opened.error());
			}
			if (!diagnostic.Entity.empty())
			{
				ImGui::BeginDisabled(m_SelectionTicket != 0);
				if (ImGui::SmallButton("Select entity"))
				{
					auto ticket = context.Actions.Submit("edit.select", Json{ { "entities", Json::array({ diagnostic.Entity }) }, { "ifRevision", m_ReportRevision } });
					if (ticket)
						m_SelectionTicket = *ticket;
					else
						report(ticket.error());
				}
				ImGui::EndDisabled();
			}
			ImGui::PopID();
		}
		return {};
	}

}
