#include "EditorPCH.h"
#include "Editor/FolderPicker.h"

#include "Engine/Core/FileSystem.h"

#include <imgui.h>

#include <algorithm>

namespace Engine {

	Status FolderPicker::Navigate(const std::filesystem::path& directory)
	{
		ENGINE_TRY_ASSIGN(const FileInfo info, FileSystem::GetInfo(directory));
		if (!info.IsDirectory)
			return MakeError(ErrorCode::NotFound, "folder picker path is not a directory");
		std::error_code error;
		auto canonical = std::filesystem::canonical(directory, error);
		if (error)
			return MakeError(ErrorCode::Io, "cannot resolve folder: {}", error.message());
		ENGINE_TRY_ASSIGN(const auto entries, FileSystem::ListDirectory(canonical));
		std::vector<std::filesystem::path> directories;
		for (const auto& entry : entries)
		{
			ENGINE_TRY_ASSIGN(const FileInfo entryInfo, FileSystem::GetInfo(entry));
			if (entryInfo.IsDirectory)
				directories.push_back(entry);
		}
		const std::string text = FileSystem::PathToUtf8(canonical);
		if (text.size() >= m_PathInput.size())
			return MakeError(ErrorCode::InvalidArgument, "folder path exceeds the picker's input capacity");
		m_PathInput.fill(0);
		std::copy(text.begin(), text.end(), m_PathInput.begin());
		m_Directory = std::move(canonical);
		m_Directories = std::move(directories);
		m_Error.clear();
		return {};
	}

	Status FolderPicker::Open(std::string_view title, const std::filesystem::path& initialDirectory)
	{
		if (title.empty())
			return MakeError(ErrorCode::InvalidArgument, "folder picker needs a title");
		ENGINE_TRY(Navigate(initialDirectory));
		m_Title = title;
		m_Filter.fill(0);
		m_Open = true;
		return {};
	}

	Result<std::optional<std::filesystem::path>> FolderPicker::Draw()
	{
		if (!m_Open)
			return std::nullopt;
		std::optional<std::filesystem::path> selected;
		std::optional<Error> failure;
		ImGui::PushID(this);
		ImGui::SetNextWindowSize(ImVec2(660.0f, 440.0f), ImGuiCond_FirstUseEver);
		const std::string window = m_Title + "###FolderPicker" + std::to_string(reinterpret_cast<uintptr_t>(this));
		if (ImGui::Begin(window.c_str(), &m_Open))
		{
			const auto navigate = [this, &failure](const std::filesystem::path& directory)
			{
				if (const Status result = Navigate(directory); !result)
				{
					failure = result.error();
					m_Error = result.error().ToString();
				}
			};
			if (ImGui::Button("Up"))
				navigate(m_Directory.parent_path());
			ImGui::SameLine();
			if (ImGui::Button("Refresh"))
				navigate(m_Directory);
			if (ImGui::InputText("Path", m_PathInput.data(), m_PathInput.size(), ImGuiInputTextFlags_EnterReturnsTrue))
				navigate(FileSystem::PathFromUtf8(m_PathInput.data()));
			ImGui::InputText("Filter folders", m_Filter.data(), m_Filter.size());
			std::optional<std::filesystem::path> next;
			if (ImGui::BeginChild("Folders", ImVec2(0.0f, -70.0f), ImGuiChildFlags_Borders))
			{
				for (const auto& directory : m_Directories)
				{
					const std::string name = FileSystem::PathToUtf8(directory.filename());
					if (!name.contains(m_Filter.data()))
						continue;
					ImGui::PushID(name.c_str());
					const ImVec2 position = ImGui::GetCursorScreenPos();
					if (ImGui::Selectable("##Directory", false))
						next = directory;
					ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
			if (next)
				navigate(*next);
			if (ImGui::Button("Select folder"))
			{
				navigate(m_Directory);
				if (!failure)
				{
					selected = m_Directory;
					m_Open = false;
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				Cancel();
			if (!m_Error.empty())
				ImGui::TextWrapped("%s", m_Error.c_str());
		}
		ImGui::End();
		ImGui::PopID();
		if (failure)
			return std::unexpected(*failure);
		return selected;
	}

	void FolderPicker::Cancel()
	{
		m_Open = false;
	}
	bool FolderPicker::IsOpen() const
	{
		return m_Open;
	}

}
