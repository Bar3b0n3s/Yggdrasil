#include "TestsPCH.h"

#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>

// The Animation category (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 28): the contract registers the Animator
// and the SkinnedMesh, so these cases are live; their behaviour is frozen by the skipped cases of the systems that read them.

namespace Engine {

	namespace {

		const ComponentInfo& RequireAnimationComponent(const TypeRegistry& registry, std::string_view name)
		{
			const ComponentInfo* info = registry.FindComponent(name);
			REQUIRE(info != nullptr);
			return *info;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("AnimationRegistration: the animation components are registered after Scripting with their requirements")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& script = RequireAnimationComponent(*registry, "Script");
			const ComponentInfo& animator = RequireAnimationComponent(*registry, "Animator");
			const ComponentInfo& skinned = RequireAnimationComponent(*registry, "SkinnedMesh");
			// ADR 0028's phase-2 ledger appends each milestone's categories after Scripting in landing order, and M23 lands last,
			// so M21's and M22's categories lie between Script and M23's on the merged tree: the relative order and M23's own
			// chain only, never an absolute neighbour that a sibling's block takes. Whatever lies between belongs to a sibling's
			// category of its own, neither one before Scripting nor M23's.
			CHECK(animator.GetIndex() > script.GetIndex());
			const std::span<const ComponentInfo* const> components = registry->GetComponents();
			for (size_t index = script.GetIndex() + 1; index < animator.GetIndex() && index < components.size(); ++index)
			{
				CAPTURE(components[index]->GetName());
				for (const std::string_view category : { "Core", "Rendering", "Physics", "Audio", "Scripting", "Animation" })
					CHECK(components[index]->GetCategory() != category);
			}
			CHECK(skinned.GetIndex() == animator.GetIndex() + 1);
			for (const ComponentInfo* info : { &animator, &skinned })
			{
				CAPTURE(info->GetName());
				CHECK(info->GetCategory() == "Animation");
				CHECK(info->GetVersion() == 1);
				REQUIRE(info->GetRequires().size() == 1);
				CHECK(info->GetExcludes().empty());
			}
			CHECK(animator.GetRequires()[0] == registry->FindComponent("Transform"));
			CHECK(skinned.GetRequires()[0] == registry->FindComponent("MeshRenderer"));

			REQUIRE(animator.FindField("Skeleton") != nullptr);
			CHECK(animator.FindField("Skeleton")->GetKind() == FieldType::AssetRef);
			CHECK(animator.FindField("Skeleton")->GetMeta().AssetFilter == "Skeleton");
			REQUIRE(animator.FindField("Layers") != nullptr);
			CHECK(animator.FindField("Layers")->GetKind() == FieldType::Array);
			CHECK(animator.FindField("Playing")->GetKind() == FieldType::Bool);
			CHECK(animator.FindField("RootMotion")->GetKind() == FieldType::Enum);
			REQUIRE(skinned.FindField("MorphWeights") != nullptr);
			CHECK(skinned.FindField("MorphWeights")->GetKind() == FieldType::Array);

			const StructInfo* layer = registry->FindStruct("AnimatorLayer");
			REQUIRE(layer != nullptr);
			REQUIRE(layer->FindField("Clip") != nullptr);
			CHECK(layer->FindField("Clip")->GetMeta().AssetFilter == "AnimationClip");
			REQUIRE(layer->FindField("Weight")->GetMeta().Max.has_value());
			CHECK(*layer->FindField("Weight")->GetMeta().Max == 1.0);
			REQUIRE(layer->FindField("Speed")->GetMeta().Max.has_value());
			CHECK(*layer->FindField("Speed")->GetMeta().Max == static_cast<double>(MaxAnimatorSpeed));
			CHECK(layer->FindField("Loop")->GetKind() == FieldType::Bool);
		}

		TEST_CASE("AnimationRegistration: an Animator holds at most eight layers and a SkinnedMesh finite weights in [-1, 1]")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			ResolveContext resolve;
			resolve.Registry = registry.get();
			const ComponentInfo& animatorInfo = RequireAnimationComponent(*registry, "Animator");
			const ComponentInfo& skinnedInfo = RequireAnimationComponent(*registry, "SkinnedMesh");
			const auto countErrors = [&resolve](const ComponentInfo& info, const void* component)
			{
				ValidationContext context;
				info.Validate(component, resolve, context);
				return context.GetErrorCount();
			};

			AnimatorComponent animator;
			CHECK(countErrors(animatorInfo, &animator) == 0);
			animator.Layers.resize(MaxAnimatorLayers);
			CHECK(countErrors(animatorInfo, &animator) == 0);
			animator.Layers.resize(MaxAnimatorLayers + 1);
			CHECK(countErrors(animatorInfo, &animator) == 1);

			SkinnedMeshComponent skinned;
			CHECK(countErrors(skinnedInfo, &skinned) == 0);
			skinned.MorphWeights = { -1.0f, 0.0f, 0.5f, 1.0f };
			CHECK(countErrors(skinnedInfo, &skinned) == 0);
			skinned.MorphWeights = { 1.5f };
			CHECK(countErrors(skinnedInfo, &skinned) == 1);
			skinned.MorphWeights = { std::numeric_limits<float>::quiet_NaN() };
			CHECK(countErrors(skinnedInfo, &skinned) == 1);
			skinned.MorphWeights.assign(MaxMeshMorphTargets + 1, 0.0f);
			CHECK(countErrors(skinnedInfo, &skinned) == 1);
		}

		// M41 (Docs/Decisions/0058-m41-bone-attachments.md decision 3): the BoneAttachment follows the SkinnedMesh in Animation,
		// requires Transform, and its fields are the colliders' types; its Bone names a joint as a Skeleton can, at most
		// MaxSkeletonJointNameBytes of valid UTF-8 (empty is valid: reported, never refused).
		TEST_CASE("AnimationRegistration: a BoneAttachment follows the SkinnedMesh, requires Transform and holds a Bone of at most 256 bytes of UTF-8")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& skinned = RequireAnimationComponent(*registry, "SkinnedMesh");
			const ComponentInfo& attachment = RequireAnimationComponent(*registry, "BoneAttachment");
			CHECK(attachment.GetIndex() == skinned.GetIndex() + 1);
			CHECK(attachment.GetCategory() == "Animation");
			CHECK(attachment.GetVersion() == 1);
			REQUIRE(attachment.GetRequires().size() == 1);
			CHECK(attachment.GetRequires()[0] == registry->FindComponent("Transform"));
			REQUIRE(attachment.FindField("Bone") != nullptr);
			CHECK(attachment.FindField("Bone")->GetKind() == FieldType::String);
			CHECK(attachment.FindField("Offset")->GetKind() == FieldType::Vec3);
			CHECK(attachment.FindField("Offset")->GetMeta().Unit == "m");
			CHECK(attachment.FindField("Rotation")->GetKind() == FieldType::Quat);
			for (const std::string_view field : { "Bone", "Offset", "Rotation" })
			{
				CAPTURE(field);
				CHECK(attachment.FindField(field)->GetDescription().find("a joint of the Animator's Skeleton (a bone)") != std::string::npos);
			}

			ResolveContext resolve;
			resolve.Registry = registry.get();
			const auto countErrors = [&resolve, &attachment](const BoneAttachmentComponent& component)
			{
				ValidationContext context;
				attachment.Validate(&component, resolve, context);
				return context.GetErrorCount();
			};
			BoneAttachmentComponent component;
			CHECK(countErrors(component) == 0);
			component.Bone = "DEF-hand.R";
			CHECK(countErrors(component) == 0);
			component.Bone.assign(MaxSkeletonJointNameBytes, 'b');
			CHECK(countErrors(component) == 0);
			component.Bone.assign(MaxSkeletonJointNameBytes + 1, 'b');
			CHECK(countErrors(component) == 1);
			component.Bone = std::string("hand\xff");
			CHECK(countErrors(component) == 1);
		}
	}

}
