#pragma once

#include "EditorCore/Automation/ProjectMethods.h"
#include "Engine/Core/Result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

namespace Engine {

	struct EditorPanelContext;

	// Asset/validation/type diagnostics with stable IDs and severity filters. Auto-fix submits project.validate
	// for the chosen IDs as one undo step. Latest import checks and the active play session's script errors update live.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class DiagnosticsPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		ProjectValidateResult m_Report{};
		std::array<char, 256> m_Search{};
		std::set<std::string> m_Selected{};
		std::filesystem::path m_Project{};
		uint64_t m_Ticket = 0;
		uint64_t m_SelectionTicket = 0;
		uint64_t m_ReportRevision = 0;
		int m_Severity = 0;
		bool m_HasReport = false;
		bool m_Refresh = true;
		bool m_Fixing = false;
		std::string m_Error{};
	};

}
