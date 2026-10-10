#include "EditorPCH.h"
#include "Editor/FolderPicker.h"

#include "Editor/Icons.h"
#include "Editor/Ui/EditorStyle.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"

#include <imgui.h>

#include <algorithm>

namespace Engine {

	namespace Utils {

		static std::string PickerFold(std::string text)
		{
			for (char& character : text)
				if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character + ('a' - 'A'));
			return text;
		}

	}

	Status FolderPicker::Navigate(const std::filesystem::path& directory)
	{
		ENGINE_TRY_ASSIGN(const FileInfo info, FileSystem::GetInfo(directory));
		if (!info.IsDirectory)
			return MakeError(ErrorCode::NotFound, "Choose a folder to browse.");
		std::error_code error;
		auto canonical = std::filesystem::canonical(directory, error);
		if (error)
			return MakeError(ErrorCode::Io, "Cannot open this folder: {}", error.message());
		ENGINE_TRY_ASSIGN(const auto entries, FileSystem::ListDirectory(canonical));
		std::vector<std::filesystem::path> directories;
		std::vector<std::filesystem::path> files;
		for (const auto& entry : entries)
		{
			ENGINE_TRY_ASSIGN(const FileInfo entryInfo, FileSystem::GetInfo(entry));
			if (entryInfo.IsDirectory)
				directories.push_back(entry);
			else
				files.push_back(entry);
		}
		const std::string text = FileSystem::PathToUtf8(canonical);
		if (text.size() >= m_PathInput.size())
			return MakeError(ErrorCode::InvalidArgument, "This folder path is too long to display.");
		m_PathInput.fill(0);
		std::copy(text.begin(), text.end(), m_PathInput.begin());
		m_Directory = std::move(canonical);
		m_Directories = std::move(directories);
		m_Files = std::move(files);
		m_SelectedFile.clear();
		m_Error.clear();
		return {};
	}

	Result<std::filesystem::path> FolderPicker::ValidateFile(const std::filesystem::path& path) const
	{
		ENGINE_TRY_ASSIGN(const FileInfo info, FileSystem::GetInfo(path));
		if (info.IsDirectory)
			return MakeError(ErrorCode::InvalidArgument, "Choose a file, or open the folder to browse its contents.");
		if (!m_Extension.empty() && !Utils::PickerFold(FileSystem::PathToUtf8(path.filename())).ends_with(m_Extension))
			return MakeError(ErrorCode::InvalidArgument, "Choose a {} file.", m_Extension);
		std::error_code error;
		auto canonical = std::filesystem::canonical(path, error);
		if (error)
			return MakeError(ErrorCode::Io, "Cannot open this file: {}", error.message());
		return canonical;
	}

	Status FolderPicker::Open(std::string_view title, const std::filesystem::path& initialDirectory)
	{
		if (title.empty())
			return MakeError(ErrorCode::InvalidArgument, "folder picker needs a title");
		ENGINE_TRY(Navigate(initialDirectory));
		m_Title = title;
		m_Filter.fill(0);
		m_Extension.clear();
		m_FileMode = false;
		m_Open = true;
		m_Appearing = true;
		return {};
	}

	Status FolderPicker::OpenFile(std::string_view title, const std::filesystem::path& initialDirectory, std::string_view extension)
	{
		if (!extension.empty() && (extension.front() != '.' || extension.size() == 1 || extension.find_first_of("/\\*?:") != std::string_view::npos))
			return MakeError(ErrorCode::InvalidArgument, "File filter must be a dot-prefixed extension.");
		ENGINE_TRY(Open(title, initialDirectory));
		m_Extension = Utils::PickerFold(std::string(extension));
		m_FileMode = true;
		return {};
	}

	Result<std::optional<std::filesystem::path>> FolderPicker::Draw()
	{
		if (!m_Open)
			return std::nullopt;
		std::optional<std::filesystem::path> selected;
		std::optional<Error> failure;
		const auto fail = [this, &failure](const Error& error)
		{
			failure = error;
			m_Error = error.GetMessageText();
		};
		const auto navigate = [this, &fail](const std::filesystem::path& directory)
		{
			if (const Status result = Navigate(directory); !result)
				fail(result.error());
			else
				m_Filter.fill(0);
		};
		const auto acceptFile = [this, &fail, &selected]()
		{
			const auto result = ValidateFile(m_SelectedFile);
			if (!result)
				fail(result.error());
			else
			{
				selected = *result;
				m_Open = false;
			}
		};
		const float scale = Utils::EditorUiScale();
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		if (m_Appearing)
		{
			ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(std::min(760.0f * scale, viewport->WorkSize.x * 0.9f), std::min(540.0f * scale, viewport->WorkSize.y * 0.9f)));
			ImGui::SetNextWindowFocus();
			m_Appearing = false;
		}
		// Launcher and asset-browser pickers may coexist. Their instance IDs isolate transient window state;
		// opening always restores placement, so these address-derived IDs must never enter the saved layout.
		const std::string window = m_Title + "###FolderPicker" + std::to_string(reinterpret_cast<uintptr_t>(this));
		if (ImGui::Begin(window.c_str(), &m_Open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::BeginDisabled(m_Directory == m_Directory.root_path());
			if (Utils::EditorToolbarButton("Up", "Browse the parent folder"))
				navigate(m_Directory.parent_path());
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (Utils::EditorToolbarButton("Root", "Browse from the root of this volume"))
				navigate(m_Directory.root_path());
			ImGui::SameLine();
			if (Utils::EditorToolbarButton("Refresh", "Reload files and folders"))
				navigate(m_Directory);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::InputTextWithHint("##Location", "Paste a path and press Enter", m_PathInput.data(), m_PathInput.size(), ImGuiInputTextFlags_EnterReturnsTrue))
			{
				auto path = FileSystem::PathFromUtf8(m_PathInput.data());
				if (path.is_relative())
					path = m_Directory / path;
				const auto info = FileSystem::GetInfo(path);
				if (!info)
					fail(info.error());
				else if (info->IsDirectory || !m_FileMode)
					navigate(path);
				else
				{
					const auto file = ValidateFile(path);
					if (!file)
						fail(file.error());
					else
					{
						navigate(file->parent_path());
						if (!failure)
							m_SelectedFile = *file;
					}
				}
			}
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputTextWithHint("##Filter", m_FileMode ? "Search files and folders..." : "Search folders...", m_Filter.data(), m_Filter.size());
			const std::string filter = Utils::PickerFold(m_Filter.data());
			std::optional<std::filesystem::path> next;
			bool openSelected = false;
			const float footer = ImGui::GetFrameHeightWithSpacing() + 2.0f * ImGui::GetTextLineHeightWithSpacing();
			if (ImGui::BeginChild("Entries", ImVec2(0.0f, std::max(ImGui::GetFrameHeight() * 2.0f, ImGui::GetContentRegionAvail().y - footer)), ImGuiChildFlags_Borders))
			{
				size_t visible = 0;
				const auto row = [this, &filter, &visible, &next, &openSelected](const std::filesystem::path& path, bool directory)
				{
					const std::string name = FileSystem::PathToUtf8(path.filename());
					const std::string folded = Utils::PickerFold(name);
					if (!folded.contains(filter) || (!directory && !m_Extension.empty() && !folded.ends_with(m_Extension)))
						return;
					++visible;
					ImGui::PushID(static_cast<int>(FNV1a32(name)));
					const ImVec2 position = ImGui::GetCursorScreenPos();
					const float height = ImGui::GetFrameHeight();
					const bool selectedFile = !directory && m_SelectedFile == path;
					if (ImGui::Selectable("##Entry", selectedFile, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, height)))
					{
						if (directory)
							next = path;
						else
						{
							m_SelectedFile = path;
							m_Error.clear();
							// A folder click and its first file can share coordinates. Only a second click
							// on the already selected file accepts; entering a folder clears that selection.
							openSelected = selectedFile && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
						}
					}
					const ImVec2 maximum = ImGui::GetItemRectMax();
					ImDrawList* draw = ImGui::GetWindowDrawList();
					draw->PushClipRect(position, maximum, true);
					if (directory)
						DrawEditorIcon(*draw, EditorIcon::Folder, glm::vec2(position.x, position.y + ImGui::GetStyle().FramePadding.y), ImGui::GetFontSize(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
					draw->AddText(ImVec2(position.x + height, position.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
					draw->PopClipRect();
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", FileSystem::PathToUtf8(path).c_str());
					ImGui::PopID();
				};
				for (const auto& directory : m_Directories)
					row(directory, true);
				if (m_FileMode)
					for (const auto& file : m_Files)
						row(file, false);
				if (visible == 0)
					Utils::EditorEmptyState(filter.empty() ? "This folder is empty" : "No matches", "Choose another folder or clear the search.");
			}
			ImGui::EndChild();
			if (next)
				navigate(*next);
			if (openSelected)
				acceptFile();
			const std::string caption = m_FileMode ? (m_SelectedFile.empty() ? "Choose a file to continue" : FileSystem::PathToUtf8(m_SelectedFile.filename())) : FileSystem::PathToUtf8(m_Directory.filename());
			ImGui::TextUnformatted(caption.c_str());
			ImGui::BeginDisabled(m_FileMode && m_SelectedFile.empty());
			if (ImGui::Button(m_FileMode ? "Open file" : "Select folder"))
			{
				if (m_FileMode)
					acceptFile();
				else
				{
					navigate(m_Directory);
					if (!failure)
					{
						selected = m_Directory;
						m_Open = false;
					}
				}
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)))
				Cancel();
			if (!m_Error.empty())
				ImGui::TextWrapped("%s", m_Error.c_str());
		}
		ImGui::End();
		if (failure)
			return std::unexpected(*failure);
		return selected;
	}

	void FolderPicker::Cancel()
	{
		m_Open = false;
		m_SelectedFile.clear();
	}

	bool FolderPicker::IsOpen() const
	{
		return m_Open;
	}

}
