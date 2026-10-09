#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Methods/RaycastMethods.h"
#include "Engine/Automation/Methods/ScreenshotMethods.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	struct ViewportPixelSize
	{
		uint32_t Width = 640;
		uint32_t Height = 360;
	};

	struct ViewportPickParams
	{
		uint32_t X = 0;                          // required framebuffer pixel, top-left origin; samples (x+.5,y+.5)
		uint32_t Y = 0;                          // required; NOT normalized or ImGui logical coordinates
		ViewportView View = ViewportView::Scene; // required
		SceneTarget Target = SceneTarget::Edit;  // presence-aware like scene.raycast
	};

	struct ViewportPickResult
	{
		ViewportView View = ViewportView::Scene;
		uint32_t Width = 0; // actual view used, includes headless stored extent
		uint32_t Height = 0;
		SceneRaycastResult Raycast{};
	};

	namespace Automation {

		// CPU only, deterministic without a graphics device. Scene view uses context.GetSceneViewCamera; game uses the
		// target's primary camera. Size is the current viewport framebuffer extent supplied by M10's EditorContext;
		// headless hosts retain a documented 640x360 default, never infer from a last screenshot. Ray through pixel centre
		// uses ComputeViewPixelRayInterval and passes BOTH distances to RaycastScene; FarClip is view-axis depth,
		// never a normalized ray distance. Orthographic distance starts at its near-plane origin.
		// InvalidArgument at /x or /y for out-of-range pixel; InvalidState for zero/minimized extent or no primary camera;
		// target/asset errors as scene.raycast. It returns a hit; it does not alter editor selection/history.
		[[nodiscard]] Result<ViewportPickResult> ViewportPick(EditorMethodContext& context, const ViewportPickParams& params);

	}

	void RegisterViewportPickMethodTypes(TypeRegistry& registry);
	// viewport.pick: read-only, batchable, SupportsDryRun, not launcher/Runtime, not MCP tool. RegisterEditorMethods owns wiring.
	void RegisterViewportPickMethods(MethodRegistry& methods);

}
