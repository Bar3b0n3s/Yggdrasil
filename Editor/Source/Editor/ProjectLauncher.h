#pragma once

#include "Editor/FileDialog.h"
#include "Engine/Core/Result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

	struct EditorPanelContext;

	// Where the launcher's folder browsers start when no location is typed and no recent project is known: the user's
	// home folder (HOME, else USERPROFILE, when it names an existing absolute directory), else the current directory.
	// Never the engine's own resource tree. Empty when none of them can be determined.
	[[nodiscard]] std::filesystem::path GetDefaultProjectLocation();

	// Recent projects, New Empty/Basic3D, Open, and recovery offers; shown only without an open project. The recent list
	// is the host's memory snapshot (EditorPanelContext::GetRecentProjects), taken again each time the launcher reappears;
	// drawing never reads user://Editor.json.
	// Main thread in ImGui. Paths come from the FileDialog API (Editor/FileDialog.h): the operating system's dialog when
	// the panel context has a native backend (a windowed editor; Windows only, Docs/Decisions/0029-m18-contract.md
	// decision 19), otherwise the ImGui folder picker. The location is a folder dialog; the project file an open-file dialog
	// for .eproj. Either dialog only fills the path that project.create and project.open take.
	// Opens once with recover=false. After open, EditorLayer shows the injected recovery offer for that held project;
	// acceptance uses Autosave::Recover at a safe point, not another project.open. Decline only dismisses the offer.
	// Automation may instead choose recover=true on its initial project.open; the launcher never retries an open.
	// Lock errors identify the owning process; invalid input/errors stay visible. Does not change cwd.
	class ProjectLauncher
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		FileDialog m_Dialog{};
		std::array<char, 4096> m_Path{};
		std::array<char, 4096> m_Location{};
		std::array<char, 256> m_Name{};
		std::vector<std::filesystem::path> m_Recent{};
		std::optional<uint64_t> m_Ticket{};
		std::string m_Error{};
		bool m_RecentLoaded = false;
		bool m_Basic3D = true;
		bool m_PickingLocation = false;
	};

}
