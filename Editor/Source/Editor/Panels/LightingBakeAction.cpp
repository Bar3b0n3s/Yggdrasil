#include "EditorPCH.h"
#include "Editor/Panels/LightingBakeAction.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"

#include <imgui.h>

namespace Engine {

	bool IsLightingBakeAvailable(const EditorContext& editor)
	{
		const EditorPlayController& play = editor.GetPlay();
		return editor.HasProject() && !editor.IsReadOnly() && editor.HasScene() && editor.GetScenePath().has_value() && !play.IsPlaying()
			&& !play.IsTestRunActive() && editor.GetProbeCapture() != nullptr;
	}

	bool DrawLightingBakeMenuItem(const EditorContext& editor)
	{
		const bool chosen = ImGui::MenuItem("Bake lighting", nullptr, false, IsLightingBakeAvailable(editor));
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("Render the saved scene's irradiance volumes and reflection probes, write their data to Assets/Lighting/<scene path>/ "
							  "and set each probe's Data (undoable); needs a rendering editor, outside play");
		}
		return chosen;
	}

}
