#include "EnginePCH.h"
#include "Engine/Scene/BoneAttachments.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Assert.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/BoneAttachmentComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TerrainColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/AnimationTargets.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The attaching pass, its findings and decision R's helpers (BoneAttachments.h). Simulation path (§4.12): only + - * /, sqrt
// (glm's products, inverses, normalize and quat casts are built from them) and Core/DetMath, so every placement is
// bit-identical in every configuration.

namespace Engine {

	namespace Utils {

		// Why an attachment is not made, or fights its placement (ANIMATION_ATTACHMENT_INVALID's causes, in the order a
		// finding names the first).
		enum class AttachmentFault : uint8_t
		{
			NoAnimator,
			EmptyBone,
			NoPose,
			MissingJoint,
			Character,
			Dynamic,
			Colliders
		};

		// Whether `entity` holds a solid (non-trigger) collider component: BoxCollider, SphereCollider, CapsuleCollider,
		// MeshCollider or TerrainCollider (always solid).
		static bool HasSolidCollider(ConstEntity entity)
		{
			const auto solid = [](const auto* collider)
			{
				return collider != nullptr && !collider->IsTrigger;
			};
			return solid(entity.TryGetComponent<BoxColliderComponent>()) || solid(entity.TryGetComponent<SphereColliderComponent>())
				|| solid(entity.TryGetComponent<CapsuleColliderComponent>()) || solid(entity.TryGetComponent<MeshColliderComponent>())
				|| entity.HasComponent<TerrainColliderComponent>();
		}

		// Whether solid colliders move with `entity`: its own, and those of its effectively enabled descendants that the
		// physics composition gathers with them (Scene/PhysicsComposition.h: no RigidBody or CharacterController of their own,
		// and none between).
		static bool HasGatheredSolidColliders(ConstEntity entity)
		{
			if (HasSolidCollider(entity))
				return true;
			const Scene& scene = *entity.GetScene();
			for (const UUID id : entity.GetChildren())
			{
				const ConstEntity child = scene.FindEntityByID(id);
				if (!child.IsValid() || !child.IsActive() || child.HasComponent<RigidBodyComponent>() || child.HasComponent<CharacterControllerComponent>())
					continue;
				if (HasGatheredSolidColliders(child))
					return true;
			}
			return false;
		}

		// The physics of `entity` that fights its placement each fixed step, if any (decision 3's "Physics").
		static std::optional<AttachmentFault> FindPhysicsFault(ConstEntity entity)
		{
			if (entity.HasComponent<CharacterControllerComponent>())
				return AttachmentFault::Character;
			const RigidBodyComponent* body = entity.TryGetComponent<RigidBodyComponent>();
			if (body != nullptr && body->Type == BodyType::Dynamic)
				return AttachmentFault::Dynamic;
			if ((body == nullptr || body->Type != BodyType::Kinematic) && HasGatheredSolidColliders(entity))
				return AttachmentFault::Colliders;
			return std::nullopt;
		}

		// What a finding needs besides its entity and fault: the Animator, its Skeleton (null when not loaded) and the bone.
		struct AttachmentContext
		{
			ConstEntity Animator{};
			const SkeletonData* Skeleton = nullptr;
			std::string_view Bone{};
		};

		// "'A', 'B' and 'C'": up to three of `names`, quoted.
		static std::string QuoteNames(std::span<const std::string> names)
		{
			std::string text;
			for (size_t index = 0; index < names.size(); ++index)
			{
				if (index > 0)
					text += index + 1 == names.size() ? " and " : ", ";
				text += std::format("'{}'", names[index]);
			}
			return text;
		}

		static AnimationSceneIssue MakeIssue(ConstEntity entity, AttachmentFault fault, const AttachmentContext& context)
		{
			const Scene& scene = *entity.GetScene();
			const std::string path = scene.GetEntityPath(entity);
			AnimationSceneIssue issue{
				.Code = std::string(AnimationAttachmentInvalidCode),
				.Severity = DiagnosticSeverity::Warning,
				.Entity = entity.GetUUID(),
				.Component = "BoneAttachment",
			};
			switch (fault)
			{
				case AttachmentFault::NoAnimator:
					issue.Message = std::format("The BoneAttachment of '{}' has no Animator above its entity; it is not attached and keeps its Transform.", path);
					issue.Hint = "Make the entity a descendant of the entity that holds the Animator (a model instance's armature entity), with "
								 "entity.reparent or Entity:SetParent.";
					break;
				case AttachmentFault::EmptyBone:
				{
					issue.Field = "Bone";
					issue.Message = std::format("The BoneAttachment of '{}' names no bone; it is not attached and keeps its Transform.", path);
					issue.Hint = "Set Bone to the name of a joint of the Animator's Skeleton (a bone), as the model names it.";
					if (context.Skeleton != nullptr && !context.Skeleton->Joints.empty())
					{
						std::vector<std::string> names;
						for (size_t index = 0; index < std::min<size_t>(context.Skeleton->Joints.size(), 3); ++index)
							names.push_back(context.Skeleton->Joints[index].Name);
						issue.Hint += std::format(" Its joints include {}.", QuoteNames(names));
					}
					break;
				}
				case AttachmentFault::NoPose:
					issue.Message = std::format("The Animator of '{}' has no pose (its Skeleton is null, does not load or poses to values that are not finite), "
												"so the BoneAttachment of '{}' is not attached and keeps its Transform.",
						scene.GetEntityPath(context.Animator), path);
					issue.Hint = "Set the Animator's Skeleton to the skeleton of the model it animates.";
					break;
				case AttachmentFault::MissingJoint:
				{
					issue.Field = "Bone";
					std::vector<std::string> names;
					if (context.Skeleton != nullptr)
					{
						for (const SkeletonJoint& joint : context.Skeleton->Joints)
							names.push_back(joint.Name);
					}
					const std::vector<std::string> suggestions = FuzzySuggest(context.Bone, std::span<const std::string>(names));
					issue.Message = std::format("The BoneAttachment of '{}' names the bone '{}', which the Skeleton of the Animator of '{}' does not hold; it is "
												"not attached and keeps its Transform.",
						path, context.Bone, scene.GetEntityPath(context.Animator));
					if (!suggestions.empty())
						issue.Message += std::format(" Did you mean {}?", QuoteNames(suggestions));
					issue.Hint = "Set Bone to the name of a joint of the Animator's Skeleton (a bone), written exactly as the model names it.";
					break;
				}
				case AttachmentFault::Character:
					issue.Message = std::format("'{}' holds a BoneAttachment and a CharacterController, whose movement fights the attachment that places the "
												"entity each fixed step.",
						path);
					issue.Hint = "Attach another entity, a child of the character's model, instead; a held object takes a Kinematic RigidBody.";
					break;
				case AttachmentFault::Dynamic:
					issue.Message = std::format("'{}' holds a BoneAttachment and a Dynamic RigidBody, which the simulation moves while the attachment places it "
												"each fixed step.",
						path);
					issue.Hint = "Set the RigidBody's Type to Kinematic: the attachment then sweeps the body with the bone, and it pushes what it meets.";
					break;
				case AttachmentFault::Colliders:
					issue.Message = std::format("The solid colliders that the BoneAttachment of '{}' moves each fixed step form no Kinematic body of its own: "
												"their body (an ancestor's compound or a static body) is rebuilt or teleported every step.",
						path);
					issue.Hint = "Give the attached entity a RigidBody of Type Kinematic, so its colliders sweep with the bone; a trigger needs none.";
					break;
			}
			return issue;
		}

		// The entities from `animator` (excluded) down to `entity` (included), `animator` a strict ancestor; root side first.
		static std::vector<ConstEntity> CollectChainBelow(ConstEntity animator, ConstEntity entity)
		{
			std::vector<ConstEntity> chain;
			for (ConstEntity current = entity; current.IsReadable() && current != animator; current = current.GetParent())
				chain.push_back(current);
			std::ranges::reverse(chain);
			return chain;
		}

		static bool IsStrictAncestor(ConstEntity ancestor, ConstEntity entity)
		{
			for (ConstEntity current = entity.GetParent(); current.IsReadable(); current = current.GetParent())
			{
				if (current == ancestor)
					return true;
			}
			return false;
		}

		static bool IsFiniteVector(const glm::vec3& vector)
		{
			return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
		}

		static bool IsFiniteQuaternion(const glm::quat& rotation)
		{
			return std::isfinite(rotation.w) && std::isfinite(rotation.x) && std::isfinite(rotation.y) && std::isfinite(rotation.z);
		}

		// Whether `a` and `b` hold the same bits (a held pose writes nothing; -0 and 0 serialize differently).
		static bool SameBits(const glm::vec3& a, const glm::vec3& b)
		{
			return std::bit_cast<uint32_t>(a.x) == std::bit_cast<uint32_t>(b.x) && std::bit_cast<uint32_t>(a.y) == std::bit_cast<uint32_t>(b.y)
				&& std::bit_cast<uint32_t>(a.z) == std::bit_cast<uint32_t>(b.z);
		}

		static bool SameBits(const glm::quat& a, const glm::quat& b)
		{
			return std::bit_cast<uint32_t>(a.w) == std::bit_cast<uint32_t>(b.w) && SameBits(glm::vec3(a.x, a.y, a.z), glm::vec3(b.x, b.y, b.z));
		}

		// The local Translation and Rotation that put `entity` (a descendant of `animator`) at the pose P, Q of the joint
		// matrix `joint` (both in the Animator entity's space) with its attachment's Offset and Rotation (the file comment's
		// steps); nullopt when the joint, the chain or the result does not decompose or is not finite.
		static std::optional<TransformDecomposition> PlaceOnJoint(ConstEntity animator, ConstEntity entity, const BoneAttachmentComponent& attachment,
			const glm::mat4& joint)
		{
			const Result<TransformDecomposition> bone = TransformSystem::DecomposeMatrix(joint);
			if (!bone.has_value())
				return std::nullopt;
			TransformDecomposition placed;
			placed.Translation = bone->Translation + bone->Rotation * attachment.Offset;
			placed.Rotation = glm::normalize(bone->Rotation * attachment.Rotation);
			const ConstEntity parent = entity.GetParent();
			if (parent != animator)
			{
				glm::mat4 chain(1.0f);
				for (const ConstEntity link : CollectChainBelow(animator, parent))
					chain = chain * TransformSystem::ComputeLocalMatrix(link.GetComponent<TransformComponent>());
				glm::mat4 target = glm::mat4_cast(placed.Rotation);
				target[3] = glm::vec4(placed.Translation, 1.0f);
				const Result<TransformDecomposition> local = TransformSystem::DecomposeMatrix(glm::affineInverse(chain) * target);
				if (!local.has_value())
					return std::nullopt;
				placed.Translation = local->Translation;
				placed.Rotation = local->Rotation;
			}
			if (!IsFiniteVector(placed.Translation) || !IsFiniteQuaternion(placed.Rotation))
				return std::nullopt;
			return placed;
		}

	}

	std::vector<AnimationSceneIssue> FindBoneAttachmentIssues(const Scene& scene, AssetManager& assets)
	{
		std::vector<AnimationSceneIssue> issues;
		if (scene.GetRegistry().view<BoneAttachmentComponent>().empty())
			return issues;
		scene.ForEachCanonical([&issues, &assets](ConstEntity entity)
		{
			const BoneAttachmentComponent* attachment = entity.TryGetComponent<BoneAttachmentComponent>();
			if (attachment == nullptr || !entity.IsActive())
				return;
			Utils::AttachmentContext context{ .Bone = attachment->Bone };
			context.Animator = FindDrivingAnimator(entity.GetParent());
			std::optional<Utils::AttachmentFault> fault;
			AssetRef<SkeletonData> skeleton;
			if (!context.Animator.IsValid())
			{
				fault = Utils::AttachmentFault::NoAnimator;
			}
			else
			{
				skeleton = Detail::LoadAnimationAsset<SkeletonData>(&assets, context.Animator.GetComponent<AnimatorComponent>().Skeleton.GetHandle());
				context.Skeleton = skeleton.get();
				if (attachment->Bone.empty())
					fault = Utils::AttachmentFault::EmptyBone;
				else if (skeleton != nullptr && !FindSkeletonJoint(*skeleton, attachment->Bone).has_value())
					fault = Utils::AttachmentFault::MissingJoint;
				else
					fault = Utils::FindPhysicsFault(entity);
			}
			if (fault.has_value())
				issues.push_back(Utils::MakeIssue(entity, *fault, context));
		});
		return issues;
	}

	std::optional<uint32_t> BoneAttachmentPass::FindJoint(const AssetRef<SkeletonData>& skeleton, std::string_view name)
	{
		auto entry = std::ranges::find_if(m_Skeletons, [&skeleton](const SkeletonJoints& candidate)
		{
			return candidate.Skeleton.get() == skeleton.get();
		});
		if (entry == m_Skeletons.end())
		{
			m_Skeletons.push_back(SkeletonJoints{ .Skeleton = skeleton });
			entry = std::prev(m_Skeletons.end());
		}
		entry->Used = true;
		if (const auto found = entry->Joints.find(name); found != entry->Joints.end())
			return found->second;
		const std::optional<uint32_t> joint = FindSkeletonJoint(*skeleton, name);
		if (joint.has_value())
			entry->Joints.emplace(std::string(name), *joint);
		return joint;
	}

	void BoneAttachmentPass::Run(Scene& scene, std::vector<AnimationSceneIssue>& findings)
	{
		entt::registry& registry = scene.GetRegistry();
		// The state marks exactly the entities this pass attaches (unregistered: neither counted nor recorded).
		registry.clear<BoneAttachmentStateComponent>();
		if (registry.view<BoneAttachmentComponent>().empty())
		{
			m_Skeletons.clear();
			return;
		}
		const std::span<const UUID> current = scene.GetCanonicalOrder();
		const std::vector<UUID> order(current.begin(), current.end());
		// Past the AnimationSystem's MaxAnimationSystemDiagnostics nothing more is kept or logged, so nothing more is built or
		// remembered either: a script that spawns broken attachments every tick cannot grow the set.
		const auto report = [this, &findings](ConstEntity entity, Utils::AttachmentFault fault, const Utils::AttachmentContext& context)
		{
			if (m_Reported.size() < MaxAnimationSystemDiagnostics && m_Reported.insert(entity.GetUUID()).second)
				findings.push_back(Utils::MakeIssue(entity, fault, context));
		};
		for (const UUID id : order)
		{
			const Entity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || !entity.HasComponent<BoneAttachmentComponent>() || entity.HasComponent<HierarchyDisabledTag>())
				continue;
			const BoneAttachmentComponent attachment = entity.GetComponent<BoneAttachmentComponent>();
			Utils::AttachmentContext context{ .Bone = attachment.Bone };
			context.Animator = FindDrivingAnimator(entity.GetParent());
			if (!context.Animator.IsValid())
			{
				report(entity, Utils::AttachmentFault::NoAnimator, context);
				continue;
			}
			const AnimationPoseComponent* pose = context.Animator.TryGetComponent<AnimationPoseComponent>();
			context.Skeleton = pose != nullptr ? pose->Skeleton.get() : nullptr;
			if (attachment.Bone.empty())
			{
				report(entity, Utils::AttachmentFault::EmptyBone, context);
				continue;
			}
			if (context.Skeleton == nullptr)
			{
				report(entity, Utils::AttachmentFault::NoPose, context);
				continue;
			}
			const std::optional<uint32_t> joint = FindJoint(pose->Skeleton, attachment.Bone);
			if (!joint.has_value() || *joint >= pose->JointModel.size())
			{
				report(entity, Utils::AttachmentFault::MissingJoint, context);
				continue;
			}
			if (!m_Reported.contains(id))
			{
				if (const std::optional<Utils::AttachmentFault> fault = Utils::FindPhysicsFault(entity))
					report(entity, *fault, context);
			}

			// Attached: placed when the pose gives a Transform, held at the one it has otherwise.
			if (const std::optional<TransformDecomposition> placed = Utils::PlaceOnJoint(context.Animator, entity, attachment, pose->JointModel[*joint]))
			{
				const TransformComponent& transform = entity.GetComponent<TransformComponent>();
				if (!Utils::SameBits(transform.Translation, placed->Translation) || !Utils::SameBits(transform.Rotation, placed->Rotation))
				{
					entity.Patch<TransformComponent>([&placed](TransformComponent& written)
					{
						written.Translation = placed->Translation;
						written.Rotation = placed->Rotation;
					});
				}
			}
			registry.emplace<BoneAttachmentStateComponent>(entity.GetHandle(), BoneAttachmentStateComponent{ .Animator = context.Animator.GetUUID(), .Joint = *joint });
		}
		std::erase_if(m_Skeletons, [](const SkeletonJoints& entry)
		{
			return !entry.Used;
		});
		for (SkeletonJoints& entry : m_Skeletons)
			entry.Used = false;
	}

	ConstEntity FindAttachedRenderAnimator(ConstEntity entity)
	{
		const Scene& scene = *entity.GetScene();
		if (!scene.IsRuntime() || scene.GetRegistry().view<BoneAttachmentStateComponent>().empty())
			return {};
		for (ConstEntity current = entity; current.IsReadable(); current = current.GetParent())
		{
			const BoneAttachmentStateComponent* state = current.TryGetComponent<BoneAttachmentStateComponent>();
			if (state == nullptr)
				continue;
			const ConstEntity animator = scene.FindEntityByID(state->Animator);
			if (animator.IsReadable() && Utils::IsStrictAncestor(animator, current))
				return animator;
		}
		return {};
	}

	glm::mat4 ComputeAttachedRelativeMatrix(ConstEntity animator, ConstEntity entity)
	{
		ENGINE_CORE_ASSERT(Utils::IsStrictAncestor(animator, entity), "ComputeAttachedRelativeMatrix needs an ancestor of the entity");
		glm::mat4 relative(1.0f);
		for (const ConstEntity link : Utils::CollectChainBelow(animator, entity))
			relative = relative * TransformSystem::ComputeLocalMatrix(link.GetComponent<TransformComponent>());
		return relative;
	}

}
