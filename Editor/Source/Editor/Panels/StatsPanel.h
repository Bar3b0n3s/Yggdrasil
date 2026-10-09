#pragma once

#include "Engine/Automation/Methods/StatsMethods.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace Engine {

	struct EditorPanelContext;

	// Read-only frame/GPU-pass/draw/memory/body/voice metrics through existing services and M9 stats.get adapter.
	// Missing GPU/script services display unavailable, never fabricated zero measurements; no clock-based UI test oracle.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class StatsPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		StatsGetResult m_Stats{};
		std::filesystem::path m_Project{};
		uint64_t m_Ticket = 0;
		bool m_HasSample = false;
		bool m_Refresh = true;
		std::string m_Error{};
	};

}
