#include "EnginePCH.h"
#include "Engine/Scene/Private/AnimationTargets.h"

#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/MeshDeform.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <format>
#include <utility>

// The Animator's SkinnedMeshes (AnimationTargets.h).

namespace Engine {

	namespace Detail {

		std::optional<std::string> GetAnimationTargetPath(ConstEntity animator, ConstEntity entity)
		{
			std::vector<std::string_view> names;
			for (ConstEntity current = entity; current.IsReadable(); current = current.GetParent())
			{
				if (current == animator)
				{
					std::string path;
					for (auto name = names.rbegin(); name != names.rend(); ++name)
					{
						if (!path.empty())
							path += '/';
						path += *name;
					}
					return path;
				}
				names.push_back(current.GetName());
			}
			return std::nullopt;
		}

		ConstEntity FindAnimationTarget(ConstEntity animator, std::string_view target)
		{
			ConstEntity current = animator;
			const Scene* scene = animator.GetScene();
			while (!target.empty() && current.IsReadable())
			{
				const size_t separator = target.find('/');
				const std::string_view name = target.substr(0, separator);
				ConstEntity next{};
				for (const UUID child : current.GetChildren())
				{
					const ConstEntity candidate = scene->FindEntityByID(child);
					if (candidate.IsValid() && candidate.GetName() == name)
					{
						next = candidate;
						break;
					}
				}
				current = next;
				target = separator == std::string_view::npos ? std::string_view() : target.substr(separator + 1);
			}
			return current.IsReadable() ? current : ConstEntity();
		}

		std::optional<std::vector<float>> ResolveSkinnedMeshWeights(ConstEntity entity, AssetManager* assets)
		{
			if (assets == nullptr || !entity.IsReadable())
				return std::nullopt;
			const SkinnedMeshComponent* skinned = entity.TryGetComponent<SkinnedMeshComponent>();
			const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>();
			if (skinned == nullptr || renderer == nullptr)
				return std::nullopt;
			const AssetRef<MeshData> mesh = LoadAnimationAsset<MeshData>(assets, renderer->Mesh.GetHandle());
			if (mesh == nullptr)
				return std::nullopt;
			std::vector<float> defaults;
			defaults.reserve(mesh->MorphTargets.size());
			for (const MeshMorphTarget& target : mesh->MorphTargets)
				defaults.push_back(target.DefaultWeight);
			return ResolveMorphWeights(defaults, skinned->MorphWeights);
		}

		std::vector<float> MergeAnimatedMorphWeights(std::span<const float> component, std::span<const float> animated)
		{
			std::vector<float> weights(std::max(component.size(), animated.size()), 0.0f);
			for (size_t index = 0; index < weights.size(); ++index)
				weights[index] = index < animated.size() ? animated[index] : component[index];
			return weights;
		}

		const AnimationMorphWeights* FindPoseMorphWeights(std::span<const AnimationMorphWeights> morphs, std::string_view target)
		{
			const auto found = std::ranges::find(morphs, target, &AnimationMorphWeights::Target);
			return found == morphs.end() ? nullptr : &*found;
		}

		std::optional<AnimationSceneIssue> FindSkinnedMeshIssue(ConstEntity entity, AssetManager& assets)
		{
			const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>();
			if (renderer == nullptr || !entity.HasComponent<SkinnedMeshComponent>())
				return std::nullopt;
			const AssetHandle meshHandle = renderer->Mesh.GetHandle();
			const AssetRef<MeshData> mesh = LoadAnimationAsset<MeshData>(&assets, meshHandle);
			if (mesh == nullptr || mesh->SkinJointCount == 0)
				return std::nullopt;
			const Scene& scene = *entity.GetScene();
			const ConstEntity animator = FindDrivingAnimator(entity);
			if (!animator.IsValid())
			{
				return AnimationSceneIssue{
					.Code = std::string(AnimationNoAnimatorCode),
					.Severity = DiagnosticSeverity::Warning,
					.Entity = entity.GetUUID(),
					.Component = "SkinnedMesh",
					.Message = std::format("The SkinnedMesh of '{}' deforms the skinned mesh '{}', but no Animator is on the entity or an ancestor; it draws "
										   "its bind pose.",
						scene.GetEntityPath(entity), assets.GetReferencePath(meshHandle)),
					.Hint = "Add an Animator with the mesh's Skeleton to the entity or an ancestor, or remove the SkinnedMesh.",
				};
			}
			const AssetHandle skeletonHandle = animator.GetComponent<AnimatorComponent>().Skeleton.GetHandle();
			const AssetRef<SkeletonData> skeleton = LoadAnimationAsset<SkeletonData>(&assets, skeletonHandle);
			if (skeleton == nullptr || skeleton->Joints.size() >= mesh->SkinJointCount)
				return std::nullopt;
			return AnimationSceneIssue{
				.Code = std::string(AnimationSkeletonMismatchCode),
				.Severity = DiagnosticSeverity::Warning,
				.Entity = entity.GetUUID(),
				.Component = "SkinnedMesh",
				.Message = std::format("The skin of the mesh '{}' that the SkinnedMesh of '{}' deforms addresses {} joints, but the Skeleton '{}' of the "
									   "Animator on '{}' holds {}; it draws its bind pose.",
					assets.GetReferencePath(meshHandle), scene.GetEntityPath(entity), mesh->SkinJointCount, assets.GetReferencePath(skeletonHandle),
					scene.GetEntityPath(animator), skeleton->Joints.size()),
				.Hint = "Set the Animator's Skeleton to the skin the mesh was imported with.",
			};
		}

	}

}
