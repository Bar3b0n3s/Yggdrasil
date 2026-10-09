#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	struct EditorStateResult
	{
		std::vector<std::string> OpenPanels{}; // EditorPanelToString, stable enum order; actually shown panels
		std::string Mode = "Edit";             // Edit, Play, Simulate, Paused
		std::vector<EntitySummary> Selection{};
		SceneTarget SelectionTarget = SceneTarget::Edit;
		std::string LockstepOwner{};
		uint32_t UiFrame = 0; // ToAutomationCounter, no fabricated frames for --renderer none
		bool SceneChangedOnDisk = false;
	};

	namespace Automation {

		// editor.state: UI-independent observation, reads current selection/play state, not a cached ImGui interpretation.
		[[nodiscard]] Result<EditorStateResult> EditorState(EditorMethodContext& context, const NoParams& params);

	}

	void RegisterEditorStateMethodTypes(TypeRegistry& registry);
	// Read-only, AllowedInBatch, not a tool, no dry run, not launcher/Runtime; --renderer none is supported.
	void RegisterEditorStateMethods(MethodRegistry& methods);

}
