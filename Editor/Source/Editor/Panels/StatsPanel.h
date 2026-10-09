#pragma once

#include "Engine/Core/Result.h"

namespace Engine {

	struct EditorPanelContext;

	// Read-only frame/GPU-pass/draw/memory/body/voice metrics through existing services and M9 stats.get adapter.
	// Missing GPU/script services display unavailable, never fabricated zero measurements; no clock-based UI test oracle.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class StatsPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	};

}
