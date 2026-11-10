#include "EditorPCH.h"
#include "EditorCore/Automation/AnimationMethods.h"

#include "EditorCore/Animation/AnimationPreview.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <format>
#include <optional>

// animation.preview (Docs/Decisions/0034-m23-contract.md decision 32): the editor's animation preview as a method, over the
// EditorContext's AnimationPreview.

namespace Engine {

	namespace Automation {

		Result<AnimationPreviewResult> AnimationPreviewMethod(EditorMethodContext& context, const AnimationPreviewParams& params)
		{
			const bool timeGiven = context.HasParam("time");
			const bool playingGiven = context.HasParam("playing");
			if (params.Stop && (timeGiven || playingGiven))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, timeGiven ? "/time" : "/playing",
					"stop ends the preview, so it takes no time or playing", "send stop alone, or time and playing without stop"));
			}
			if (timeGiven && (!std::isfinite(params.Time) || params.Time < 0.0f || params.Time > MaxAnimatorTime))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/time",
					std::format("an animation preview time must be finite and in [0, {}] s, not {}", MaxAnimatorTime, params.Time)));
			}

			// The open edit scene the request addresses: its scene target, else the active scene. While playing, a request
			// without a scene target (or naming the played scene) reads the play scene, whose session poses its Animators
			// itself.
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(SceneTarget::Edit, false, false));
			if (const PlaySession* session = context.GetPlaySession(); session != nullptr && scene == &session->GetScene())
			{
				return std::unexpected(Error(ErrorCode::InvalidState, "animation.preview poses an edit scene, and the play session poses its own Animators")
						.WithHint("end the session with play.stop first, or name another open scene with the scene target"));
			}
			ENGINE_TRY_ASSIGN(const OpenSceneId sceneId, context.ResolveSceneTarget());
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(*scene, params.Entity, "/entity"));
			if (!entity.HasComponent<AnimatorComponent>())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/entity",
					std::format("'{}' holds no Animator to preview", scene->GetEntityPath(entity)),
					"preview the entity that holds the Animator: an imported glTF's Animator is on the skin's common ancestor"));
			}

			AnimationPreview& preview = editor.GetAnimationPreview();
			const std::optional<AnimationPreviewState>& running = preview.GetState();
			const bool same = running.has_value() && running->Scene == sceneId && running->Entity == entity.GetUUID();
			AnimationPreviewResult result{ .Entity = context.MakeEntitySummary(entity) };
			if (params.Stop)
			{
				// Only a preview of this scene's entity ends; any other keeps running.
				if (same)
					preview.End(editor);
				return result;
			}

			// An absent time or playing keeps the running preview's, when it previews this scene's entity.
			const float time = timeGiven ? params.Time : (same ? running->Time : 0.0f);
			const bool playing = playingGiven ? params.Playing : (same && running->Playing);
			// The pose reads the settled assets: imports and hot-reload swaps requested so far are published first (§8.13).
			AssetManager& assets = editor.GetAssets();
			assets.WaitIdle();
			ENGINE_TRY_ASSIGN(const AnimationPreviewState state, preview.Begin(editor, sceneId, entity.GetUUID(), assets, time, playing));
			result.Previewing = true;
			result.Time = state.Time;
			result.Playing = state.Playing;
			result.Duration = state.Duration;
			return result;
		}

	}

	void RegisterAnimationMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<AnimationPreviewParams>("AnimationPreviewParams", "The params of animation.preview.")
			.Field("entity", &AnimationPreviewParams::Entity, "The entity whose Animator to preview: its id, a unique id prefix or its path.")
			.Field("time", &AnimationPreviewParams::Time,
				"Seconds added to every layer's time, times the layer's speed, at most a layer time's own bound; absent keeps the running "
				"preview's (0 for a new one).",
				{ .Min = 0.0, .Max = static_cast<double>(MaxAnimatorTime), .Unit = "s" })
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
