#include "EditorPCH.h"
#include "Editor/ProjectLauncher.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Icons.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"

#include <imgui.h>

#include <algorithm>

namespace Engine {

	Status ProjectLauncher::Draw(EditorPanelContext& context)
	{
		const auto showError = [this](const Error& error)
		{
			m_Error = error.GetMessageText();
			ENGINE_ERROR("Project launcher: {}", error.ToString());
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
				if (!**completed && (**completed).error().GetCode() != ErrorCode::Cancelled)
					showError((**completed).error());
				m_Ticket.reset();
				m_RecentLoaded = false;
			}
		}
		if (context.Editor.HasProject())
		{
			m_Picker.Cancel();
			return {};
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
		const float scale = Utils::EditorUiScale();
		const ImVec2 available = ImGui::GetContentRegionAvail();
		const float width = std::min(840.0f * scale, available.x);
		const float height = std::min(560.0f * scale, available.y);
		const ImVec2 origin = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(origin.x + std::max(0.0f, (available.x - width) * 0.5f), origin.y + std::max(0.0f, (available.y - height) * 0.35f)));
		if (ImGui::BeginChild("ProjectLanding", ImVec2(width, height), ImGuiChildFlags_Borders))
		{
			ImGui::Dummy(ImVec2(0.0f, 8.0f * scale));
			const ImVec2 icon = ImGui::GetCursorScreenPos();
			DrawEditorIcon(*ImGui::GetWindowDrawList(), EditorIcon::Scene, glm::vec2(icon.x, icon.y), ImGui::GetFontSize() * 2.2f, ImGui::GetColorU32(ImGuiCol_CheckMark));
			ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 2.2f, ImGui::GetFontSize() * 2.2f));
			ImGui::SameLine();
			ImGui::BeginGroup();
			ImGui::TextUnformatted("Build your next world.");
			ImGui::TextDisabled("Pick up where you left off, or start something new.");
			ImGui::EndGroup();
			ImGui::Spacing();
			ImGui::BeginDisabled(m_Ticket.has_value());
			if (ImGui::BeginTabBar("ProjectChoices"))
			{
				if (ImGui::BeginTabItem("Recent"))
				{
					ImGui::Spacing();
					Utils::EditorSectionHeading("Recent projects");
					if (m_Recent.empty())
						Utils::EditorEmptyState("A fresh start", "Create your first project in New project, or choose Open project to browse your files. Your recent projects will appear here.");
					else if (ImGui::BeginChild("RecentProjects", ImVec2(0.0f, ImGui::GetFrameHeightWithSpacing() * 7.0f)))
					{
						for (const auto& path : m_Recent)
						{
							const std::string text = FileSystem::PathToUtf8(path);
							const std::string name = FileSystem::PathToUtf8(path.stem());
							ImGui::PushID(static_cast<int>(FNV1a32(text)));
							const ImVec2 position = ImGui::GetCursorScreenPos();
							const float rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
							if (ImGui::Selectable("##RecentProject", false, ImGuiSelectableFlags_None, ImVec2(0.0f, rowHeight)))
								submit("project.open", Json{ { "path", text }, { "recover", false } });
							const ImVec2 maximum = ImGui::GetItemRectMax();
							ImDrawList* draw = ImGui::GetWindowDrawList();
							draw->PushClipRect(position, maximum, true);
							const float inset = ImGui::GetStyle().FramePadding.x;
							draw->AddText(ImVec2(position.x + inset, position.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
							draw->AddText(ImVec2(position.x + inset, position.y + ImGui::GetStyle().FramePadding.y + ImGui::GetTextLineHeightWithSpacing()), ImGui::GetColorU32(ImGuiCol_TextDisabled), text.c_str());
							draw->PopClipRect();
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("Open %s\n%s", name.c_str(), text.c_str());
							ImGui::PopID();
						}
					}
					if (!m_Recent.empty())
						ImGui::EndChild();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("New project"))
				{
					ImGui::Spacing();
					Utils::EditorSectionHeading("Start with a template");
					const bool wide = ImGui::GetContentRegionAvail().x > 520.0f * scale;
					if (ImGui::BeginTable("Templates", wide ? 2 : 1, ImGuiTableFlags_SizingStretchSame))
					{
						for (int index = 0; index < 2; ++index)
						{
							ImGui::TableNextColumn();
							ImGui::PushID(index);
							const bool basic = index == 0;
							if (ImGui::Selectable(basic ? "3D starter" : "Empty project", m_Basic3D == basic, ImGuiSelectableFlags_None, ImVec2(0.0f, ImGui::GetFrameHeight())))
								m_Basic3D = basic;
							ImGui::TextWrapped("%s", basic ? "Camera, lighting and ground. Ready to explore." : "A clean project for a world of your own.");
							ImGui::PopID();
						}
						ImGui::EndTable();
					}
					ImGui::Spacing();
					ImGui::TextUnformatted("Project name");
					ImGui::SetNextItemWidth(-1.0f);
					ImGui::InputTextWithHint("##ProjectName", "My project", m_Name.data(), m_Name.size());
					ImGui::TextUnformatted("Location");
					const float browseWidth = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
					ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - browseWidth - ImGui::GetStyle().ItemSpacing.x));
					ImGui::InputTextWithHint("##ProjectLocation", "Choose a parent folder", m_Location.data(), m_Location.size());
					ImGui::SameLine();
					if (ImGui::Button("Browse..."))
					{
						const auto initial = m_Location[0] ? FileSystem::PathFromUtf8(m_Location.data()) : context.Editor.GetSpecification().TemplatesDirectory.parent_path();
						if (const Status opened = m_Picker.Open("Choose a location for your project", initial); !opened)
							showError(opened.error());
						else
							m_PickingLocation = true;
					}
					const std::string name = m_Name.data();
					const bool validName = !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string::npos;
					const auto destination = FileSystem::PathFromUtf8(m_Location.data()) / FileSystem::PathFromUtf8(name);
					ImGui::TextWrapped("%s", m_Location[0] && validName ? ("Creates " + FileSystem::PathToUtf8(destination)).c_str() : "A new folder will be created for your project.");
					ImGui::BeginDisabled(!validName || m_Location[0] == '\0');
					if (ImGui::Button("Create project"))
						submit("project.create", Json{ { "path", FileSystem::PathToUtf8(destination) }, { "name", name }, { "template", m_Basic3D ? "Basic3D" : "Empty" } });
					ImGui::EndDisabled();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Open project"))
				{
					ImGui::Spacing();
					Utils::EditorSectionHeading("Continue an existing project");
					ImGui::TextWrapped("Choose a project file to open its scenes and assets.");
					if (ImGui::Button("Browse projects..."))
					{
						const auto browse = [this](std::filesystem::path directory) -> Status
						{
							// Recent projects may have moved or lived on an unavailable volume. Walk to a
							// browsable ancestor, keeping expected intermediate failures out of the UI.
							for (;;)
							{
								const Status opened = m_Picker.OpenFile("Open a project", directory, ".eproj");
								if (opened)
									return {};
								const auto parent = directory.parent_path();
								if (parent.empty() || parent == directory)
									return opened;
								directory = parent;
							}
						};
						const auto fallback = context.Editor.GetSpecification().TemplatesDirectory.parent_path();
						const auto initial = m_Recent.empty() ? fallback : m_Recent.front().parent_path();
						Status opened = browse(initial);
						if (!opened && initial != fallback)
							opened = browse(fallback);
						if (!opened)
							showError(opened.error());
						else
						{
							m_Error.clear();
							m_PickingLocation = false;
						}
					}
					ImGui::Spacing();
					ImGui::TextUnformatted("Or paste a project file or folder path");
					ImGui::SetNextItemWidth(-1.0f);
					ImGui::InputTextWithHint("##OpenProjectPath", "Path to a project", m_Path.data(), m_Path.size());
					ImGui::BeginDisabled(m_Path[0] == '\0');
					if (ImGui::Button("Open project"))
						submit("project.open", Json{ { "path", m_Path.data() }, { "recover", false } });
					ImGui::EndDisabled();
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			ImGui::EndDisabled();
			if (m_Ticket)
			{
				ImGui::Separator();
				ImGui::TextUnformatted("Preparing your project...");
				ImGui::SameLine();
				if (ImGui::SmallButton("Cancel operation"))
					if (const Status cancelled = context.Actions.Cancel(*m_Ticket); !cancelled)
						showError(cancelled.error());
			}
			if (!m_Error.empty())
			{
				ImGui::Separator();
				Utils::EditorSectionHeading("Couldn't open the project");
				ImGui::TextWrapped("%s", m_Error.c_str());
			}
		}
		ImGui::EndChild();
		if (m_Picker.IsOpen())
		{
			auto picked = m_Picker.Draw();
			if (!picked)
				showError(picked.error());
			else if (*picked)
			{
				const std::string path = FileSystem::PathToUtf8(**picked);
				auto& target = m_PickingLocation ? m_Location : m_Path;
				if (path.size() >= target.size())
					showError(Error(ErrorCode::InvalidArgument, "The selected path is too long."));
				else
				{
					target.fill(0);
					std::copy(path.begin(), path.end(), target.begin());
					if (!m_PickingLocation && !m_Ticket)
						submit("project.open", Json{ { "path", path }, { "recover", false } });
				}
			}
		}
		return {};
	}

}
