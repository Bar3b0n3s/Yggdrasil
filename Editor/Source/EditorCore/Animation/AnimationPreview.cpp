#include "EditorPCH.h"
#include "EditorCore/Animation/AnimationPreview.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <utility>

// The editor's animation preview (AnimationPreview.h; Docs/Decisions/0034-m23-contract.md decision 32). The pose is the
// AnimationSystem's PoseAnimator, written into the runtime AnimationPoseComponent of the previewed entity only, so nothing
// the scene serializes, hashes or records changes.

namespace Engine {

	namespace Utils {

		// What the preview's slider and its playback need from an Animator's layers: the longest Duration of the clips that
		// load (0 without one), and whether every layer with such a clip loops.
		struct AnimationPreviewClips
		{
			float Duration = 0.0f;
			bool AllLoop = true;
		};

		static AnimationPreviewClips DescribePreviewClips(const AnimatorComponent& animator, AssetManager& assets)
		{
			AnimationPreviewClips clips;
			for (const AnimatorLayer& layer : animator.Layers)
			{
				const AssetHandle handle = layer.Clip.GetHandle();
				if (!handle.IsValid())
					continue;
				Result<AssetRef<Asset>> loaded = assets.Load(handle);
				if (!loaded.has_value())
					continue;
				const AssetRef<AnimationClipData> clip = AssetCast<AnimationClipData>(*loaded);
				if (clip == nullptr)
					continue;
				clips.Duration = std::max(clips.Duration, clip->Duration);
				clips.AllLoop = clips.AllLoop && layer.Loop;
			}
			return clips;
		}

		// The preview's time `seconds` later: wrapped at `clips`' Duration when every layer loops (computed in double), else
		// held there; unchanged without a clip to play.
		[[nodiscard]] static float AdvancePreviewTime(float time, double seconds, const AnimationPreviewClips& clips)
		{
			if (clips.Duration <= 0.0f)
				return time;
			const double duration = static_cast<double>(clips.Duration);
			const double advanced = static_cast<double>(time) + seconds;
			if (!clips.AllLoop)
				return static_cast<float>(std::min(advanced, duration));
			const double wrapped = advanced - std::floor(advanced / duration) * duration;
			// Rounding can leave the wrapped time at the duration itself, the start of the next cycle.
			return wrapped >= duration ? 0.0f : static_cast<float>(wrapped);
		}

		static void RemovePreviewPose(Entity entity)
		{
			if (entity.IsValid() && entity.HasComponent<AnimationPoseComponent>())
				entity.RemoveComponent<AnimationPoseComponent>();
		}

	}

	Result<AnimationPreviewState> AnimationPreview::Begin(EditorContext& context, OpenSceneId scene, UUID entity, AssetManager& assets, float time,
		bool playing)
	{
		if (!std::isfinite(time) || time < 0.0f || time > MaxAnimatorTime)
			return MakeError(ErrorCode::InvalidArgument, "an animation preview time must be finite and in [0, {}] s, not {}", MaxAnimatorTime, time);
		Scene* target = context.FindScene(scene);
		if (target == nullptr)
			return MakeError(ErrorCode::NotFound, "no open scene has the id {}", scene);
		Entity animator = target->FindEntityByID(entity);
		if (!animator.IsValid())
			return MakeError(ErrorCode::NotFound, "the scene '{}' has no entity {}", target->GetName(), entity.ToString());
		if (!animator.HasComponent<AnimatorComponent>())
		{
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("'{}' holds no Animator to preview", animator.GetName()))
					.WithHint("preview the entity that holds the Animator: an imported glTF's Animator is on the skin's common ancestor"));
		}

		// A preview of another entity, or of an entity of the same id in another scene, ends first: its pose leaves its own
		// scene.
		if (m_State.has_value() && (m_State->Scene != scene || m_State->Entity != entity))
			End(context);
		const Status posed = PoseAnimator(animator, assets, time);
		if (!posed)
		{
			Utils::RemovePreviewPose(animator);
			m_State.reset();
			return std::unexpected(posed.error());
		}
		const Utils::AnimationPreviewClips clips = Utils::DescribePreviewClips(animator.GetComponent<AnimatorComponent>(), assets);
		m_State = AnimationPreviewState{
			.Scene = scene,
			.ActiveScene = context.GetActiveSceneId(),
			.Entity = entity,
			.Time = time,
			.Playing = playing,
			.Duration = clips.Duration,
		};
		return *m_State;
	}

	void AnimationPreview::Update(EditorContext& context, AssetManager& assets, double frameSeconds)
	{
		if (!m_State.has_value())
			return;
		ENGINE_ASSERT(std::isfinite(frameSeconds) && frameSeconds >= 0.0, "a frame's seconds are finite and at least 0");
		AnimationPreviewState& state = *m_State;
		// A closed or replaced scene took the pose with it.
		Scene* scene = context.FindScene(state.Scene);
		if (scene == nullptr)
		{
			m_State.reset();
			return;
		}
		const Entity animator = scene->FindEntityByID(state.Entity);
		if (context.GetActiveSceneId() != state.ActiveScene || !animator.IsValid() || !animator.HasComponent<AnimatorComponent>())
		{
			End(context);
			return;
		}

		// The Animator's fields may have changed since the last frame (an inspector edit, an undo), so the clips and the pose
		// are taken again every frame.
		const Utils::AnimationPreviewClips clips = Utils::DescribePreviewClips(animator.GetComponent<AnimatorComponent>(), assets);
		state.Duration = clips.Duration;
		if (state.Playing)
			state.Time = Utils::AdvancePreviewTime(state.Time, frameSeconds, clips);
		const Status posed = PoseAnimator(animator, assets, state.Time);
		if (!posed)
		{
			ENGINE_WARN("Animation preview of '{}' ended: {}", animator.GetName(), posed.error());
			End(context);
		}
	}

	void AnimationPreview::End(EditorContext& context)
	{
		if (!m_State.has_value())
			return;
		const AnimationPreviewState state = *std::exchange(m_State, std::nullopt);
		if (Scene* scene = context.FindScene(state.Scene))
			Utils::RemovePreviewPose(scene->FindEntityByID(state.Entity));
	}

}
