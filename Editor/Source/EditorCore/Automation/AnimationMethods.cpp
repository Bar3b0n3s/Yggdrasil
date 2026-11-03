#include "EditorPCH.h"
#include "EditorCore/Automation/AnimationMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// animation.preview (Docs/Decisions/0034-m23-contract.md decision 32): the wire format and the registration are the
// contract's; stream D implements the handler and calls the registration functions from RegisterMethods.cpp.

namespace Engine {

	namespace Automation {

		Result<AnimationPreviewResult> AnimationPreviewMethod(EditorMethodContext& /*context*/, const AnimationPreviewParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "animation.preview (Docs/Decisions/0034-m23-contract.md decision 32, stream D)");
		}

	}

	void RegisterAnimationMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<AnimationPreviewParams>("AnimationPreviewParams", "The params of animation.preview.")
			.Field("entity", &AnimationPreviewParams::Entity, "The entity whose Animator to preview: its id, a unique id prefix or its path.")
			.Field("time", &AnimationPreviewParams::Time,
				"Seconds added to every layer's time, times the layer's speed; absent keeps the running preview's (0 for a new one).",
				{ .Min = 0.0, .Unit = "s" })
			.Field("playing", &AnimationPreviewParams::Playing,
				"Whether the editor's frames advance the preview's time; absent keeps the running preview's (false for a new one).")
			.Field("stop", &AnimationPreviewParams::Stop, "End the preview, so the entity draws its bind pose again; time and playing are then refused.");
		registry.Struct<AnimationPreviewResult>("AnimationPreviewResult", "The preview after an animation.preview.")
			.Field("entity", &AnimationPreviewResult::Entity, "The previewed entity.")
			.Field("previewing", &AnimationPreviewResult::Previewing, "Whether a preview runs; false after stop.")
			.Field("time", &AnimationPreviewResult::Time, "The preview's time in seconds.", { .Unit = "s" })
			.Field("playing", &AnimationPreviewResult::Playing, "Whether the editor's frames advance the time.")
			.Field("duration", &AnimationPreviewResult::Duration, "The longest loaded layer clip's duration in seconds (0 without one).",
				{ .Unit = "s" });
	}

	void RegisterAnimationMethods(MethodRegistry& methods)
	{
		const Json example = { { "entity", "/Hero" }, { "time", 0.5 }, { "playing", false } };
		methods.Add(
			{
				.Name = "animation.preview",
				.Description = "Shows an edit scene's Animator posed at a time without playing or changing the scene (no undo step, nothing "
							   "saved), or ends the preview with stop; scene views, thumbnails and viewport.screenshot show the pose. "
							   "Edit scenes otherwise draw skinned meshes in their bind pose.",
				.RequiredParams = { "entity" },
				.ExposeAsTool = true,
				.SupportsSceneTarget = true,
				.Examples = { { .Description = "Pose the hero half a second into its clips.", .Params = example } },
			},
			&Automation::AnimationPreviewMethod);
	}

}
