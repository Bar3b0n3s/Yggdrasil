#include "EnginePCH.h"
#include "Engine/Scene/AnimationSystem.h"

#include "Engine/Animation/AnimationPose.h"
#include "Engine/Animation/AnimationSampling.h"
#include "Engine/Animation/RootMotion.h"
#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/AnimationTargets.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

// The AnimationSystem phase and the pose queries (AnimationSystem.h).

namespace Engine {

	namespace Utils {

		// One layer of an Animator in a step: its clip (null when it does not load) and its time step.
		struct AnimatorLayerStep
		{
			AssetRef<AnimationClipData> Clip{};
			AnimationTimeStep Time{};
		};

		// The layers of `animator` with their clips, each time advanced by `seconds` (finite, >= 0) times the layer's Speed
		// (0: the layer's time brought into its clip), at most MaxAnimatorLayers. The delta is computed in double and kept
		// finite in float (AdvanceAnimationTime's precondition), whatever the two give: the session's fixed delta and the
		// preview's time (at most MaxAnimatorTime) never reach the bound.
		static std::vector<AnimatorLayerStep> StepLayers(const AnimatorComponent& animator, AssetManager* assets, double seconds)
		{
			const size_t count = std::min(animator.Layers.size(), static_cast<size_t>(MaxAnimatorLayers));
			std::vector<AnimatorLayerStep> layers(count);
			for (size_t index = 0; index < count; ++index)
			{
				const AnimatorLayer& layer = animator.Layers[index];
				layers[index].Clip = Detail::LoadAnimationAsset<AnimationClipData>(assets, layer.Clip.GetHandle());
				if (layers[index].Clip == nullptr)
					continue;
				const double product = seconds * static_cast<double>(layer.Speed);
				const float delta = static_cast<float>(std::min(product, static_cast<double>(std::numeric_limits<float>::max())));
				layers[index].Time = AdvanceAnimationTime(layer.Time, delta, layers[index].Clip->Duration, layer.Loop);
			}
			return layers;
		}

		// Step 2 of AnimationSystem.h's file comment: the pose of `animator` (on `entity`) over `skeleton` at its layers' times,
		// with the root motion of their steps held out of it when `holdRootMotion`; `motion` receives the blended delta.
		static AnimationPoseComponent ComputeAnimatorPose(ConstEntity entity, const AnimatorComponent& animator, const AssetRef<SkeletonData>& skeleton,
			std::span<const AnimatorLayerStep> layers, AssetManager* assets, bool holdRootMotion, RootMotionDelta& motion)
		{
			const AnimationPose rest = MakeRestPose(*skeleton);
			AnimationPose pose = rest;
			// The base of each morph target path a loaded layer keys: that entity's SkinnedMesh weights (BlendAnimationPose).
			for (const AnimatorLayerStep& layer : layers)
			{
				if (layer.Clip == nullptr)
					continue;
				for (const AnimationChannel& channel : layer.Clip->Channels)
				{
					if (channel.Path != AnimationPath::Weights || Detail::FindPoseMorphWeights(pose.Morphs, channel.Target) != nullptr)
						continue;
					std::optional<std::vector<float>> base = Detail::ResolveSkinnedMeshWeights(Detail::FindAnimationTarget(entity, channel.Target), assets);
					if (base.has_value())
						pose.Morphs.push_back(AnimationMorphWeights{ .Target = channel.Target, .Weights = std::move(*base) });
				}
			}
			motion = {};
			const uint32_t rootMotionJoint = skeleton->RootMotionJoint;
			for (size_t index = 0; index < layers.size(); ++index)
			{
				const float weight = animator.Layers[index].Weight;
				if (layers[index].Clip == nullptr || !(weight > 0.0f))
					continue;
				const AnimationClipData& clip = *layers[index].Clip;
				AnimationPose layerPose = rest;
				SampleAnimationClip(clip, *skeleton, layers[index].Time.Time, layerPose);
				if (holdRootMotion)
				{
					const RootMotionDelta delta = ComputeRootMotion(clip, *skeleton, layers[index].Time);
					HoldRootMotion(clip, *skeleton, layerPose);
					const uint8_t keyed = rootMotionJoint < layerPose.Keyed.size() ? layerPose.Keyed[rootMotionJoint] : uint8_t{ 0 };
					BlendRootMotion(motion, delta, keyed, weight);
				}
				BlendAnimationPose(pose, layerPose, weight);
			}
			AnimationPoseComponent result;
			result.Skeleton = skeleton;
			result.JointModel = ComputeJointModelTransforms(*skeleton, pose);
			result.Skin = ComputeSkinMatrices(*skeleton, result.JointModel);
			result.Morphs = std::move(pose.Morphs);
			return result;
		}

		static bool IsFiniteMatrix(const glm::mat4& matrix)
		{
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		static bool IsFiniteVector(const glm::vec3& vector)
		{
			return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
		}

		static bool IsFiniteQuaternion(const glm::quat& rotation)
		{
			return std::isfinite(rotation.w) && std::isfinite(rotation.x) && std::isfinite(rotation.y) && std::isfinite(rotation.z);
		}

		// M23's milestone review: whether every value `pose` holds is finite (its joint model and skin matrices, its morph
		// weights and its root motion). A clip or skeleton the validators accept may hold values near the float limit, whose
		// sampling and composition overflow (keys 2e38 and -2e38 interpolate to an infinity), and such a pose is never
		// written (AnimationSystem.h, step 2).
		static bool IsPoseFinite(const AnimationPoseComponent& pose)
		{
			const auto finiteWeights = [](const AnimationMorphWeights& morph)
			{
				return std::ranges::all_of(morph.Weights, [](float weight)
				{
					return std::isfinite(weight);
				});
			};
			return std::ranges::all_of(pose.JointModel, IsFiniteMatrix) && std::ranges::all_of(pose.Skin, IsFiniteMatrix)
				&& std::ranges::all_of(pose.Morphs, finiteWeights) && IsFiniteVector(pose.RootMotion.Translation)
				&& IsFiniteQuaternion(pose.RootMotion.Rotation);
		}

		// Writes `pose` as `entity`'s runtime pose (neither counted nor recorded: RuntimeComponents.h's rules).
		static void WritePose(Entity entity, AnimationPoseComponent pose)
		{
			if (AnimationPoseComponent* existing = entity.TryGetComponent<AnimationPoseComponent>())
				*existing = std::move(pose);
			else
				entity.AddComponent<AnimationPoseComponent>(std::move(pose));
		}

		static void RemovePose(Entity entity)
		{
			if (entity.HasComponent<AnimationPoseComponent>())
				entity.RemoveComponent<AnimationPoseComponent>();
		}

	}

	struct AnimationSystem::State
	{
		Scene* RuntimeScene = nullptr;               // documented back-reference
		AssetManager* Assets = nullptr;              // documented back-reference, or null for a session without assets
		IAnimationEventListener* Listener = nullptr; // documented back-reference, or null
		std::vector<AnimationSceneIssue> Diagnostics{};
		// The (entity, code) findings raised, so each is raised and logged once; the codes are AnimationValidation.h's
		// constants.
		std::set<std::pair<UUID, std::string_view>> RaisedIssues{};
		// The Animators whose dropped events were logged (MaxAnimationEventsPerStep), each once.
		std::set<UUID> EventDropsLogged{};
		// M23's milestone review: the Animators whose pose or root motion was not finite, each logged once.
		std::set<UUID> NonFiniteLogged{};
		// The step's scratch, kept to reuse its capacity: the Animators in canonical order and the events collected.
		std::vector<UUID> Animators{};
		std::vector<AnimationEventRecord> Events{};

		void StepAnimator(Entity entity, const SimStep& step);
		// Logs `what` for the Animator on `entity` once per Animator (NonFiniteLogged).
		void LogNonFinite(Entity entity, uint64_t tick, std::string_view what);
		void RaiseIssue(AnimationSceneIssue issue);
		void RaiseSkinnedMeshIssues();
	};

	void AnimationSystem::State::StepAnimator(Entity entity, const SimStep& step)
	{
		const AnimatorComponent animator = entity.GetComponent<AnimatorComponent>();
		const UUID id = entity.GetUUID();
		std::vector<Utils::AnimatorLayerStep> layers = Utils::StepLayers(animator, Assets, animator.Playing ? step.FixedDelta : 0.0);
		if (animator.Playing)
		{
			bool advanced = false;
			uint32_t delivered = 0;
			uint64_t dropped = 0;
			std::vector<uint32_t> fired;
			for (size_t index = 0; index < layers.size(); ++index)
			{
				const AnimationClipData* clip = layers[index].Clip.get();
				if (clip == nullptr)
					continue;
				advanced = advanced || layers[index].Time.Time != animator.Layers[index].Time;
				fired.clear();
				CollectAnimationEvents(*clip, layers[index].Time, fired);
				for (const uint32_t event : fired)
				{
					if (delivered == MaxAnimationEventsPerStep)
					{
						++dropped;
						continue;
					}
					++delivered;
					Events.push_back(AnimationEventRecord{ .Entity = id,
						.Layer = static_cast<uint32_t>(index),
						.Clip = animator.Layers[index].Clip.GetHandle(),
						.Name = clip->Events[event].Name,
						.Time = clip->Events[event].Time,
						.Tick = step.Tick });
				}
			}
			if (dropped > 0 && EventDropsLogged.insert(id).second)
			{
				ENGINE_CORE_WARN("The Animator of entity {} ('{}') passed more than {} animation events in one fixed step; {} were dropped, and later "
								 "drops of this Animator are not logged",
					id.ToString(), RuntimeScene->GetEntityPath(entity), MaxAnimationEventsPerStep, dropped);
			}
			// Through the scene (Entity::Patch), so the change tracker and the state hash see the new times.
			if (advanced)
			{
				entity.Patch<AnimatorComponent>([&layers](AnimatorComponent& component)
				{
					for (size_t index = 0; index < std::min(component.Layers.size(), layers.size()); ++index)
						if (layers[index].Clip != nullptr)
							component.Layers[index].Time = layers[index].Time.Time;
				});
			}
		}

		const AssetRef<SkeletonData> skeleton = Detail::LoadAnimationAsset<SkeletonData>(Assets, animator.Skeleton.GetHandle());
		if (skeleton == nullptr)
		{
			Utils::RemovePose(entity);
			return;
		}
		const bool holdRootMotion = animator.RootMotion != AnimatorRootMotion::None;
		RootMotionDelta motion;
		AnimationPoseComponent pose = Utils::ComputeAnimatorPose(entity, animator, skeleton, layers, Assets, holdRootMotion, motion);
		pose.RootMotion = motion;
		pose.Tick = step.Tick;
		// M23's milestone review: a pose or root motion that is not finite is never written, and the Transform keeps its value.
		if (!Utils::IsPoseFinite(pose))
		{
			Utils::RemovePose(entity);
			LogNonFinite(entity, step.Tick, "its pose is not finite; the Animator keeps no pose, and its Transform is kept");
			return;
		}
		Utils::WritePose(entity, std::move(pose));
		if (animator.RootMotion == AnimatorRootMotion::Apply && motion != RootMotionDelta{})
		{
			const TransformComponent& current = entity.GetComponent<TransformComponent>();
			const glm::vec3 translation = current.Translation + current.Rotation * (current.Scale * motion.Translation);
			const glm::quat rotation = glm::normalize(current.Rotation * motion.Rotation);
			if (!Utils::IsFiniteVector(translation) || !Utils::IsFiniteQuaternion(rotation))
			{
				LogNonFinite(entity, step.Tick, "applying its root motion would make its Transform not finite; the Transform is kept");
				return;
			}
			entity.Patch<TransformComponent>([&translation, &rotation](TransformComponent& transform)
			{
				transform.Translation = translation;
				transform.Rotation = rotation;
			});
		}
	}

	void AnimationSystem::State::LogNonFinite(Entity entity, uint64_t tick, std::string_view what)
	{
		const UUID id = entity.GetUUID();
		if (!NonFiniteLogged.insert(id).second)
			return;
		ENGINE_CORE_WARN("The Animator of entity {} ('{}') at tick {}: {} (its clips or Skeleton hold values that overflow the float range); later "
						 "such steps of this Animator are not logged",
			id.ToString(), RuntimeScene->GetEntityPath(entity), tick, what);
	}

	void AnimationSystem::State::RaiseIssue(AnimationSceneIssue issue)
	{
		if (Diagnostics.size() >= MaxAnimationSystemDiagnostics)
			return;
		const std::string_view code = issue.Code == AnimationNoAnimatorCode ? AnimationNoAnimatorCode : AnimationSkeletonMismatchCode;
		if (!RaisedIssues.emplace(issue.Entity, code).second)
			return;
		if (Diagnostics.size() + 1 == MaxAnimationSystemDiagnostics)
		{
			ENGINE_CORE_WARN("{} animation findings reached; later findings of this session's scene are neither kept nor logged",
				MaxAnimationSystemDiagnostics);
		}
		ENGINE_CORE_WARN("{}: {}", issue.Code, issue.Message);
		Diagnostics.push_back(std::move(issue));
	}

	void AnimationSystem::State::RaiseSkinnedMeshIssues()
	{
		if (Assets == nullptr || Diagnostics.size() >= MaxAnimationSystemDiagnostics)
			return;
		const Scene& scene = *RuntimeScene;
		const std::span<const UUID> current = scene.GetCanonicalOrder();
		const std::vector<UUID> order(current.begin(), current.end());
		for (const UUID id : order)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || !entity.HasComponent<SkinnedMeshComponent>() || entity.HasComponent<HierarchyDisabledTag>())
				continue;
			if (std::optional<AnimationSceneIssue> issue = Detail::FindSkinnedMeshIssue(entity, *Assets))
				RaiseIssue(std::move(*issue));
		}
	}

	AnimationSystem::AnimationSystem(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	AnimationSystem::~AnimationSystem() = default;

	Result<Scope<AnimationSystem>> AnimationSystem::Create(const AnimationSystemSpecification& specification)
	{
		if (specification.RuntimeScene == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the AnimationSystem needs the play session's runtime scene");
		Scope<AnimationSystem> system = CreateScope<AnimationSystem>(ConstructionKey());
		system->m_State->RuntimeScene = specification.RuntimeScene;
		system->m_State->Assets = specification.Assets;
		return system;
	}

	void AnimationSystem::Step(const SimStep& step)
	{
		State& state = *m_State;
		Scene& scene = *state.RuntimeScene;
		entt::registry& registry = scene.GetRegistry();
		// A scene without the animation components (every scene before M23) costs two empty views and changes nothing.
		const bool animators = !registry.view<AnimatorComponent>().empty();
		const bool skinned = !registry.view<SkinnedMeshComponent>().empty();
		if (!animators && !skinned)
			return;

		// Stepping: every Animator of an effectively enabled entity, in the canonical order taken now.
		state.Events.clear();
		state.Animators.clear();
		if (animators)
		{
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const Entity entity = scene.FindEntityByID(id);
				if (entity.IsValid() && entity.HasComponent<AnimatorComponent>())
					state.Animators.push_back(id);
			}
			for (const UUID id : state.Animators)
			{
				// An Animator stepped earlier cannot destroy or disable another (events wait for the dispatch), so this only
				// skips what the tick's gameplay code destroyed or disabled.
				const Entity entity = scene.FindEntityByID(id);
				if (entity.IsValid() && !entity.HasComponent<HierarchyDisabledTag>() && entity.HasComponent<AnimatorComponent>())
					state.StepAnimator(entity, step);
			}
		}
		if (skinned)
			state.RaiseSkinnedMeshIssues();

		// Dispatch: after every Animator was stepped, in (canonical entity, layer, firing) order, over the step's own list,
		// which no callback reaches (a callback's Scene.CreateEntity or Scene.Destroy changes only the scene).
		if (state.Listener == nullptr)
			return;
		for (const AnimationEventRecord& event : state.Events)
		{
			const Entity entity = scene.FindEntityByID(event.Entity);
			if (!entity.IsValid() || entity.HasComponent<HierarchyDisabledTag>())
				continue;
			state.Listener->OnAnimationEvent(event);
		}
	}

	void AnimationSystem::SetEventListener(IAnimationEventListener* listener)
	{
		m_State->Listener = listener;
	}

	std::span<const AnimationSceneIssue> AnimationSystem::GetDiagnostics() const
	{
		return m_State->Diagnostics;
	}

	Status PoseAnimator(Entity animator, AssetManager& assets, float previewTime)
	{
		ENGINE_CORE_ASSERT(animator.IsValid() && animator.HasComponent<AnimatorComponent>(), "PoseAnimator needs a valid entity with an Animator");
		if (!std::isfinite(previewTime) || previewTime < 0.0f || previewTime > MaxAnimatorTime)
			return MakeError(ErrorCode::InvalidArgument, "an animation preview time must be finite and in [0, {}] s, not {}", MaxAnimatorTime, previewTime);
		const AnimatorComponent component = animator.GetComponent<AnimatorComponent>();
		const AssetHandle skeletonHandle = component.Skeleton.GetHandle();
		const AssetRef<SkeletonData> skeleton = Detail::LoadAnimationAsset<SkeletonData>(&assets, skeletonHandle);
		if (skeleton == nullptr)
		{
			Utils::RemovePose(animator);
			if (!skeletonHandle.IsValid())
				return MakeError(ErrorCode::NotFound, "the Animator of '{}' has no Skeleton to pose", animator.GetName());
			return MakeError(ErrorCode::NotFound, "the Skeleton {} ('{}') of the Animator of '{}' does not load", skeletonHandle.ToString(),
				assets.GetReferencePath(skeletonHandle), animator.GetName());
		}
		const std::vector<Utils::AnimatorLayerStep> layers = Utils::StepLayers(component, &assets, static_cast<double>(previewTime));
		RootMotionDelta motion;
		AnimationPoseComponent pose = Utils::ComputeAnimatorPose(animator, component, skeleton, layers, &assets,
			component.RootMotion != AnimatorRootMotion::None, motion);
		// M23's milestone review: as the phase never writes a pose that is not finite, neither does the preview.
		if (!Utils::IsPoseFinite(pose))
		{
			Utils::RemovePose(animator);
			return std::unexpected(Error(ErrorCode::Validation,
				std::format("the Animator of '{}' poses to values that are not finite at {} s; it is not posed", animator.GetName(), previewTime))
					.WithHint("its clips or Skeleton hold values that overflow the float range: reimport the model with smaller translations or scales"));
		}
		Utils::WritePose(animator, std::move(pose));
		return {};
	}

	ConstEntity FindDrivingAnimator(ConstEntity entity)
	{
		for (ConstEntity current = entity; current.IsReadable(); current = current.GetParent())
			if (current.HasComponent<AnimatorComponent>())
				return current;
		return {};
	}

	std::optional<glm::mat4> FindAnimatorJointWorldTransform(ConstEntity animator, std::string_view joint)
	{
		const AnimationPoseComponent* pose = animator.TryGetComponent<AnimationPoseComponent>();
		if (pose == nullptr || pose->Skeleton == nullptr)
			return std::nullopt;
		const std::optional<uint32_t> index = FindSkeletonJoint(*pose->Skeleton, joint);
		if (!index.has_value() || *index >= pose->JointModel.size())
			return std::nullopt;
		const WorldTransformComponent* world = animator.TryGetComponent<WorldTransformComponent>();
		return (world != nullptr ? world->Matrix : TransformSystem::ComputeWorldMatrix(animator)) * pose->JointModel[*index];
	}

}
