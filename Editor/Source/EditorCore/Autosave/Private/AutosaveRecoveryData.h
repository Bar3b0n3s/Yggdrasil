#pragma once

#include "EditorCore/Autosave/Autosave.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

	class LoadedProject;

	namespace Utils {

		// Internal, CPU-only preflight shared by Autosave and project.open. No mounts or live editor mutation.
		struct AutosaveRecoveryData
		{
			AutosaveRecoveryInfo Info{};
			std::string SceneText{};
		};

		[[nodiscard]] Result<std::optional<AutosaveRecoveryData>> ReadAutosaveRecovery(const LoadedProject& project,
			std::string_view generation = {});

	}

}
