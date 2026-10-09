#pragma once

#include "Engine/Core/Result.h"

namespace Engine {

	struct EditorPanelContext;

	// Reflected ProjectSettings (window/simulation, physics layers and matrix, input action Map/Variant values,
	// export/scripting). One validated merge patch per completed edit; no duplicate hard-coded settings schema.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class ProjectSettingsPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	};

}
