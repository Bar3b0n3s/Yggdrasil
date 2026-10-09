#pragma once

#include "Editor/FolderPicker.h"
#include "Engine/Core/Result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

	struct EditorPanelContext;

	// Recent projects, New Empty/Basic3D, Open, and recovery offers; shown only without an open project.
	// Main thread in ImGui. Native paths come from the ImGui folder picker, no per-OS dialog library.
	// Opens once with recover=false. After open, EditorLayer shows the injected recovery offer for that held project;
	// acceptance uses Autosave::Recover at a safe point, not another project.open. Decline only dismisses the offer.
	// Automation may instead choose recover=true on its initial project.open; the launcher never retries an open.
	// Lock errors identify the owning process; invalid input/errors stay visible. Does not change cwd.
	class ProjectLauncher
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		FolderPicker m_Picker{};
		std::array<char, 4096> m_Path{};
		std::array<char, 256> m_Name{};
		std::vector<std::filesystem::path> m_Recent{};
		std::optional<uint64_t> m_Ticket{};
		std::string m_Error{};
		bool m_RecentLoaded = false;
		bool m_Basic3D = true;
	};

}
