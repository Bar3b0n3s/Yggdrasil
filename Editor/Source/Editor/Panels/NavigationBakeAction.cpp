#include "EditorPCH.h"
#include "Editor/Panels/NavigationBakeAction.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"

#include <imgui.h>

namespace Engine {

	bool IsNavigationBakeAvailable(const EditorContext& editor)
	{
		const EditorPlayController& play = editor.GetPlay();
		return editor.HasProject() && !editor.IsReadOnly() && editor.HasScene() && editor.GetScenePath().has_value() && !play.IsPlaying()
			&& !play.IsTestRunActive();
	}

	bool DrawNavigationBakeMenuItem(const EditorContext& editor)
	{
		const bool chosen = ImGui::MenuItem("Bake navigation", nullptr, false, IsNavigationBakeAvailable(editor));
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("Build the saved scene's navigation mesh from its static colliders and NavModifiers, write it to "
							  "Assets/Navigation/<scene path>.navmesh and set its Navigation component's NavMesh (undoable); outside play");
		}
		return chosen;
	}

}
