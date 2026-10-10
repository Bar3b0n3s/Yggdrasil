#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/Value.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace Engine {

	struct EditorPanelContext;

	// Reflected ProjectSettings (window/simulation, physics layers and matrix, input action Map/Variant values,
	// export/scripting). One validated merge patch per completed edit; no duplicate hard-coded settings schema.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class ProjectSettingsPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		ProjectSettings m_Draft{};
		Value m_Value{};
		Json m_Baseline{};
		std::filesystem::path m_Project{};
		uint64_t m_Revision = 0;
		uint64_t m_Ticket = 0;
		uint64_t m_DraftEpoch = 0; // invalidates active ImGui text buffers when a draft is discarded
		bool m_Editing = false;
		std::string m_Section{}; // empty selects the scalar general settings; other keys come from reflection
		std::string m_Error{};
	};

}
