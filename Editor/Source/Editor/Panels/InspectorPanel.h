#pragma once

#include "Engine/Core/Result.h"

namespace Engine {

	struct EditorPanelContext;

	// Registry-driven components/categories, multi-edit, add/remove, prefab override highlighting and per-field revert.
	// Uses ReflectedEditController/DrawReflectedValue including Map/Variant; asset selection shows native properties and
	// import settings. Begin on activation, commit on release, cancel ESC. Read-only controls remain visible and disabled.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class InspectorPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	};

}
