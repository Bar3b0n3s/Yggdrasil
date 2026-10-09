#pragma once

#include "Engine/Core/Result.h"
#include "EditorCore/Inspector/ReflectedEditController.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
	private:
		void ReportFailure(const Error& error);
		[[nodiscard]] Status ApplyQueuedEdit(EditorPanelContext& context);
		void CancelEdit(EditorPanelContext& context);
		std::optional<InspectorEditTarget> m_ActiveTarget{};
		std::string m_Error{};
		std::vector<uint64_t> m_Tickets{};
		bool m_CommitQueued = false;
		friend class EditorLayer;
	};

}
