#include "EditorPCH.h"
#include "Editor/Panels/DiagnosticsPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>

namespace Engine {

	namespace Utils {

		static void DrawDiagnosticMessage(DiagnosticSeverity severity, const std::string& message)
		{
			const bool error = severity == DiagnosticSeverity::Error;
			ImGui::TextColored(error ? ImVec4(0.95f, 0.48f, 0.48f, 1.0f) : ImVec4(0.91f, 0.73f, 0.39f, 1.0f), "%s", error ? "Error" : "Warning");
			ImGui::TextWrapped("%s", message.c_str());
		}

		static void DrawDiagnosticLocation(std::string_view file, uint32_t line, uint32_t column = 0)
		{
			if (file.empty())
				return;
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			const std::string text = line == 0 ? std::string(file) : column == 0 ? std::format("{}:{}", file, line)
																				 : std::format("{}:{}:{}", file, line, column);
			ImGui::TextWrapped("%s", text.c_str());
			ImGui::PopStyleColor();
		}

	}

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
		if (project.empty())
		{
			Utils::EditorEmptyState("No project open", "Open a project to check assets, settings and scripts for issues.");
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
		if (Utils::EditorToolbarButton("Validate project", "Check project assets, settings and scripts. Validation does not change the project."))
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
		if (ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 23.0f)
			ImGui::SameLine();
		ImGui::BeginDisabled(m_Ticket != 0 || m_Selected.empty() || context.Editor.IsReadOnly() || m_ReportRevision != context.Editor.GetRevision());
		if (Utils::EditorToolbarButton("Fix selected", "Apply the checked fixes as one undoable change. Refresh stale validation before fixing."))
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
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##DiagnosticSearch", "Search messages, files or codes", m_Search.data(), m_Search.size());
		ImGui::SetItemTooltip("Search ignores case. Separate alternatives with commas; prefix exclusions with a minus sign.");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::Combo("##Severity", &m_Severity, "All severities\0Errors\0Warnings\0");
		ImGui::SetItemTooltip("Filter project validation, import findings and runtime errors together.");
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		if (m_Ticket != 0)
			ImGui::TextDisabled("%s", m_Fixing ? "Applying selected fixes..." : "Validation queued");
		if (m_HasReport)
		{
			ImGui::TextWrapped("Project validation: %u errors, %u warnings", m_Report.ErrorCount, m_Report.WarningCount);
			if (m_ReportRevision != context.Editor.GetRevision())
				ImGui::TextWrapped("Project changed. Validate again before applying fixes.");
		}
		if (!m_Selected.empty())
			ImGui::TextDisabled("%zu fixes selected", m_Selected.size());
		const ImGuiTextFilter filter(m_Search.data());
		const auto visible = [this, &filter](DiagnosticSeverity severity, const std::string& text)
		{
			return (m_Severity != 1 || severity == DiagnosticSeverity::Error) && (m_Severity != 2 || severity == DiagnosticSeverity::Warning)
				&& filter.PassFilter(text.c_str());
		};
		const auto openScript = [&context, &report](std::string_view file, uint32_t line)
		{
			const auto path = VfsPath::Create("project", file);
			if (!path)
			{
				report(path.error());
				return;
			}
			const Status opened = context.OpenSource ? context.OpenSource(context.Editor.GetProject().GetRoot() / FileSystem::PathFromUtf8(path->GetPath()), line)
													 : Status(MakeError(ErrorCode::InvalidState, "source editor is not configured"));
			if (!opened)
				report(opened.error());
		};
		size_t visibleCount = 0;
		const bool showFindings = ImGui::BeginChild("DiagnosticEntries", ImVec2(0.0f, 0.0f));
		if (!showFindings)
		{
			ImGui::EndChild();
			return {};
		}
		std::set<std::string> checkedFiles;
		std::set<std::string> shownFindings;
		for (const AssetRecord* record : context.Editor.GetAssets().GetRegistry().GetRecords())
		{
			if (record->Metadata.Type != AssetType::Script)
				continue;
			const auto check = context.Editor.GetAssets().GetScriptCheck(record->Metadata.Handle);
			if (!check || !check->Performed)
				continue;
			checkedFiles.emplace(record->SourcePath.GetPath());
			for (const ScriptDiagnostic& diagnostic : check->Diagnostics)
			{
				const std::string text = std::format("{} | {}:{}:{}–{}:{} | {}", diagnostic.Code, diagnostic.File, diagnostic.Line, diagnostic.Column,
					diagnostic.EndLine, diagnostic.EndColumn, diagnostic.Message);
				if (!visible(diagnostic.Severity, text) || !shownFindings.insert(text).second)
					continue;
				++visibleCount;
				ImGui::PushID(static_cast<int>(FNV1a32(text)));
				Utils::DrawDiagnosticMessage(diagnostic.Severity, diagnostic.Message);
				Utils::DrawDiagnosticLocation(diagnostic.File, diagnostic.Line, diagnostic.Column);
				if (ImGui::SmallButton("Open source"))
					openScript(diagnostic.File, diagnostic.Line);
				ImGui::SetItemTooltip("Open the script at the reported line.");
				if (ImGui::TreeNodeEx("Details", ImGuiTreeNodeFlags_SpanAvailWidth))
				{
					ImGui::TextWrapped("Code: %s", diagnostic.Code.c_str());
					ImGui::TextWrapped("Range: %u:%u to %u:%u", diagnostic.Line, diagnostic.Column, diagnostic.EndLine, diagnostic.EndColumn);
					ImGui::TreePop();
				}
				ImGui::Separator();
				ImGui::PopID();
			}
		}
		const PlaySession* session = context.Editor.GetPlay().GetSession();
		if (session != nullptr)
		{
			for (const ScriptError& error : session->GetScriptErrors().GetErrors())
			{
				const std::string text = std::format("{} | {}:{}:{} {} | {} | {} | {} ({} occurrences)", ScriptErrorKindToString(error.Kind),
					error.Script, error.Line, error.Column, error.JsonPointer, error.EntityName, error.Callback, error.Message, error.Count);
				if (!visible(DiagnosticSeverity::Error, text))
					continue;
				++visibleCount;
				const std::string id = std::format("runtime/{}", error.ID);
				ImGui::PushID(id.c_str());
				Utils::DrawDiagnosticMessage(DiagnosticSeverity::Error, error.Message);
				Utils::DrawDiagnosticLocation(error.Script, error.Line, error.Column);
				ImGui::TextWrapped("%s", std::format("{} / {} ({} occurrences)", error.EntityName, error.Callback, error.Count).c_str());
				if (!error.Script.empty() && ImGui::SmallButton("Open source"))
					openScript(error.Script, error.Line);
				if (ImGui::TreeNodeEx("Details", ImGuiTreeNodeFlags_SpanAvailWidth))
				{
					ImGui::TextWrapped("%s", std::string(ScriptErrorKindToString(error.Kind)).c_str());
					if (!error.JsonPointer.empty())
						ImGui::TextWrapped("Field: %s", error.JsonPointer.c_str());
					for (const ScriptTraceFrame& frame : error.Traceback)
						ImGui::TextWrapped("%s:%u  %s", frame.Script.c_str(), frame.Line, frame.Function.c_str());
					ImGui::TreePop();
				}
				ImGui::Separator();
				ImGui::PopID();
			}
		}
		if (!m_HasReport)
		{
			if (visibleCount == 0)
				Utils::EditorEmptyState("Project and script diagnostics", m_Ticket != 0 ? "Checking this project. Current script findings appear as they become available." : "Validate the project to find issues and suggested fixes.");
			ImGui::EndChild();
			return {};
		}
		for (const ProjectDiagnostic& diagnostic : m_Report.Diagnostics)
		{
			if (diagnostic.Code == "SCRIPT_TYPE_ERROR" && checkedFiles.contains(diagnostic.File))
				continue; // Latest attempted import diagnostics supersede the report's older type-check snapshot.
			const std::string text = std::format("{} | {} | {} | {}", diagnostic.Id, diagnostic.Code, diagnostic.Message, diagnostic.File);
			if (!visible(diagnostic.Severity, text))
				continue;
			++visibleCount;
			ImGui::PushID(diagnostic.Id.c_str());
			Utils::DrawDiagnosticMessage(diagnostic.Severity, diagnostic.Message);
			Utils::DrawDiagnosticLocation(diagnostic.File, diagnostic.Line);
			if (diagnostic.AutoFixable)
			{
				ImGui::BeginDisabled(context.Editor.IsReadOnly() || m_Ticket != 0 || m_ReportRevision != context.Editor.GetRevision());
				bool selected = m_Selected.contains(diagnostic.Id);
				if (ImGui::Checkbox("Include in fix", &selected))
				{
					if (selected)
						m_Selected.insert(diagnostic.Id);
					else
						m_Selected.erase(diagnostic.Id);
				}
				ImGui::EndDisabled();
				ImGui::SetItemTooltip("Check this issue, then use Fix selected. All selected fixes form one undo step.");
			}
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
			if (ImGui::TreeNodeEx("Details", ImGuiTreeNodeFlags_SpanAvailWidth))
			{
				ImGui::TextWrapped("Code: %s", diagnostic.Code.c_str());
				ImGui::TextWrapped("ID: %s", diagnostic.Id.c_str());
				if (!diagnostic.Hint.empty())
					ImGui::TextWrapped("%s", diagnostic.Hint.c_str());
				ImGui::TreePop();
			}
			ImGui::Separator();
			ImGui::PopID();
		}
		if (visibleCount == 0)
			Utils::EditorEmptyState(m_Search[0] != '\0' || m_Severity != 0 ? "No matching issues" : "No issues found",
				m_Search[0] != '\0' || m_Severity != 0 ? "Adjust the search or severity filter to see more." : "Project validation and current script checks have no issues to show.");
		ImGui::EndChild();
		return {};
	}

}
