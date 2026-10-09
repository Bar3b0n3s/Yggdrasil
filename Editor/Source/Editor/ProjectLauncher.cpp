#include "EditorPCH.h"
#include "Editor/ProjectLauncher.h"

#include "Editor/EditorPanelContext.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"

#include <imgui.h>

#include <algorithm>

namespace Engine {

	Status ProjectLauncher::Draw(EditorPanelContext& context)
	{
		if (context.Editor.HasProject())
			return {};
		const auto showError = [this](const Error& error)
		{
			m_Error = error.ToString();
			ENGINE_ERROR("Project launcher: {}", m_Error);
		};
		if (m_Ticket)
		{
			auto completed = context.Actions.TakeResult(*m_Ticket);
			if (!completed)
			{
				showError(completed.error());
				m_Ticket.reset();
			}
			else if (*completed)
			{
				if (!**completed)
					showError((**completed).error());
				m_Ticket.reset();
				m_RecentLoaded = false;
			}
		}
		if (!m_RecentLoaded)
		{
			auto recent = ProjectManager::ReadRecentProjects(context.Editor.GetVfs());
			if (recent)
				m_Recent = std::move(*recent);
			else
				showError(recent.error());
			m_RecentLoaded = true;
		}
		const auto submit = [this, &context, &showError](std::string_view method, const Json& params)
		{
			auto ticket = context.Actions.Submit(method, params);
			if (!ticket)
				showError(ticket.error());
			else
			{
				m_Ticket = *ticket;
				m_Error.clear();
			}
		};
		ImGui::TextUnformatted("Open a project or create one from a template.");
		ImGui::BeginDisabled(m_Ticket.has_value());
		ImGui::InputText("Project path", m_Path.data(), m_Path.size());
		if (ImGui::Button("Browse folders"))
		{
			const auto initial = m_Path[0] ? FileSystem::PathFromUtf8(m_Path.data()) : context.Editor.GetSpecification().TemplatesDirectory;
			if (const Status opened = m_Picker.Open("Choose project folder", initial); !opened)
				showError(opened.error());
		}
		ImGui::SameLine();
		if (ImGui::Button("Open project"))
			submit("project.open", Json{ { "path", m_Path.data() }, { "recover", false } });
		ImGui::Separator();
		ImGui::InputText("New project name", m_Name.data(), m_Name.size());
		ImGui::Checkbox("Basic3D (camera, lighting and ground)", &m_Basic3D);
		ImGui::TextUnformatted("Create uses the project path above; it must be new or empty.");
		if (ImGui::Button("Create project"))
			submit("project.create", Json{ { "path", m_Path.data() }, { "name", m_Name.data() }, { "template", m_Basic3D ? "Basic3D" : "Empty" } });
		ImGui::SeparatorText("Recent projects");
		for (const auto& path : m_Recent)
		{
			const std::string text = FileSystem::PathToUtf8(path);
			ImGui::PushID(text.c_str());
			if (ImGui::Button("Open"))
				submit("project.open", Json{ { "path", text }, { "recover", false } });
			ImGui::SameLine();
			ImGui::TextUnformatted(text.c_str());
			ImGui::PopID();
		}
		ImGui::EndDisabled();
		if (m_Ticket)
		{
			ImGui::TextUnformatted("Project operation pending");
			if (ImGui::Button("Cancel operation"))
				if (const Status cancelled = context.Actions.Cancel(*m_Ticket); !cancelled)
					showError(cancelled.error());
		}
		if (m_Picker.IsOpen())
		{
			auto picked = m_Picker.Draw();
			if (!picked)
				showError(picked.error());
			else if (*picked)
			{
				const std::string path = FileSystem::PathToUtf8(**picked);
				m_Path.fill(0);
				std::copy_n(path.begin(), std::min(path.size(), m_Path.size() - 1), m_Path.begin());
			}
		}
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		return {};
	}

}
