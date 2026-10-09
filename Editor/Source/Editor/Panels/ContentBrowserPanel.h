#pragma once

#include "Engine/Core/Result.h"

namespace Engine {

	struct EditorPanelContext;

	// Project folder tree/grid, type icons, cached thumbnails and import-error badges. Create folder/scene/material/
	// prefab/sound effect through existing asset/prefab methods; script creation is explicitly unavailable until M13.
	// Rename/move preserve handles; delete uses undoable trash. OS drops use asset.import, preview audio uses AudioPreview.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class ContentBrowserPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	};

}
