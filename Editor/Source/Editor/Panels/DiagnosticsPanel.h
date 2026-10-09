#pragma once

#include "Engine/Core/Result.h"

namespace Engine {

	struct EditorPanelContext;

	// Asset/validation/type diagnostics with stable IDs and severity filters. Auto-fix submits project.validate
	// for the chosen IDs as one undo step. M13 script type-check diagnostics remain unavailable until that service exists.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class DiagnosticsPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	};

}
