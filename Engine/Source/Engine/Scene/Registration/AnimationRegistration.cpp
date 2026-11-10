#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <algorithm>
#include <cmath>
#include <format>

// The Animation category (M23, C23.3, C-SKEL, C-MORPH, C-ANIM-BLEND; Docs/Decisions/0034-m23-contract.md decision 28; ADR
// 0028's phase-2 ledger: a milestone's new components go in categories of their own after Scripting): the Animator, which
// poses a skeleton from its layers' clips in the fixed step, and the SkinnedMesh, which deforms its MeshRenderer's mesh by
// that pose and by morph weights. Registered by the contract, so every W23.3 stream builds on the final entries.
// FeatureTest's Animation.scene and the SkinnedPose golden scene hold them; no audited demo does.

namespace Engine {

	namespace Utils {

		// An Animator's blend stack holds at most MaxAnimatorLayers layers.
		static void ValidateAnimator(const AnimatorComponent& animator, ValidationContext& context)
		{
			if (animator.Layers.size() > MaxAnimatorLayers)
				context.Error("Layers", std::format("must hold at most {} layers, not {}", MaxAnimatorLayers, animator.Layers.size()));
		}

		static void MakeAnimatorValid(AnimatorComponent& animator, Random& /*random*/)
		{
			if (animator.Layers.size() > MaxAnimatorLayers)
				animator.Layers.resize(MaxAnimatorLayers);
		}

		// A SkinnedMesh holds at most MaxMeshMorphTargets morph weights, each finite in [-1, 1].
		static void ValidateSkinnedMesh(const SkinnedMeshComponent& skinned, ValidationContext& context)
		{
			if (skinned.MorphWeights.size() > MaxMeshMorphTargets)
			{
				context.Error("MorphWeights", std::format("must hold at most {} weights, not {}", MaxMeshMorphTargets, skinned.MorphWeights.size()));
			}
			for (size_t index = 0; index < skinned.MorphWeights.size(); ++index)
			{
				const float weight = skinned.MorphWeights[index];
				if (!std::isfinite(weight) || weight < -1.0f || weight > 1.0f)
					context.Error("MorphWeights", std::format("weight {} must be finite and in [-1, 1]", index));
			}
		}

		static void MakeSkinnedMeshValid(SkinnedMeshComponent& skinned, Random& /*random*/)
		{
			if (skinned.MorphWeights.size() > MaxMeshMorphTargets)
				skinned.MorphWeights.resize(MaxMeshMorphTargets);
			for (float& weight : skinned.MorphWeights)
				weight = std::isfinite(weight) ? std::clamp(weight, -1.0f, 1.0f) : 0.0f;
		}

	}

	void RegisterAnimationComponents(TypeRegistry& registry)
	{
		registry.Enum<AnimatorRootMotion>("AnimatorRootMotion", "What an Animator does with its skeleton's root motion.")
			.Entry(AnimatorRootMotion::None, "None", "The root motion stays in the pose, so the mesh moves away from the entity.")
			.Entry(AnimatorRootMotion::Apply, "Apply", "The root motion moves the entity's Transform each fixed step; the mesh stays at the entity.")
			.Entry(AnimatorRootMotion::Extract, "Extract",
				"The root motion is held out of the pose and only reported by Animator:GetRootMotion, for a script to apply.");

		registry.Struct<AnimatorLayer>("AnimatorLayer", "One layer of an Animator's blend stack: a clip played at a time, speed and weight.")
			.Field("Clip", &AnimatorLayer::Clip, "The animation clip the layer plays; null plays nothing.")
			.Field("Time", &AnimatorLayer::Time, "How far into the clip the layer is, in seconds; advanced each fixed step by the step times Speed.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxAnimatorTime), .Unit = "s" })
			.Field("Speed", &AnimatorLayer::Speed, "How fast the layer's time advances; 0 holds it.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxAnimatorSpeed) })
			.Field("Weight", &AnimatorLayer::Weight, "How much the layer's animated joints override the layers before it, from 0 (none) to 1 (fully).",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("Loop", &AnimatorLayer::Loop, "Whether the clip starts again at its end; otherwise its last pose holds.");

		RegisterComponent<AnimatorComponent>(registry, "Animator",
			"Poses a skeleton from layers of animation clips, blended by their weights, each fixed step; skinned meshes on the entity and below it follow.")
			.Category("Animation")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Skeleton", &AnimatorComponent::Skeleton, "The skeleton the layers' clips pose; null poses nothing.")
			.Field("Layers", &AnimatorComponent::Layers, "The layers, at most eight, blended in order over the skeleton's rest pose.")
			.Field("Playing", &AnimatorComponent::Playing, "Whether the layers' times advance each fixed step; the pose follows the times either way.")
			.Field("RootMotion", &AnimatorComponent::RootMotion, "What happens to the skeleton's root motion: kept in the pose, applied to the entity or only reported.")
			.Validate(&Utils::ValidateAnimator)
			.Generate(&Utils::MakeAnimatorValid);

		RegisterComponent<SkinnedMeshComponent>(registry, "SkinnedMesh",
			"Deforms the entity's MeshRenderer mesh: its skin follows the nearest Animator on the entity or above it, and its morph targets take weights.")
			.Category("Animation")
			.Version(1)
			.Requires<MeshRendererComponent>()
			.Field("MorphWeights", &SkinnedMeshComponent::MorphWeights,
				"One weight in [-1, 1] per morph target of the mesh, in order; a missing weight is the target's default, and an animated weight overrides it.")
			.Validate(&Utils::ValidateSkinnedMesh)
			.Generate(&Utils::MakeSkinnedMeshValid);
	}

}
