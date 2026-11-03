#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Skeletons (M23, C23.3, C-SKEL; Docs/Decisions/0034-m23-contract.md decisions 23 and 24): a glTF skin as a cooked asset
// (AssetType::Skeleton, format version 1), produced by GltfImporter version 3 as the sub-asset "skin:<index>[:<name>]" and
// read by the Animation module (sampling, joint and skin matrices) through an Animator. A skeleton is data only:
//   - Joints are listed parents first (a joint's Parent is -1, a root, or the index of an earlier joint), so one pass in
//     order composes every joint's model transform (Animation/AnimationPose.h). Model space is the space of the entity that
//     holds the Animator: a root joint's rest transform is relative to it.
//   - A joint's identity is its index (a skinned mesh's joint indices, MeshSkinVertex::Joints, address the skeleton's joints
//     in this order); its Name, unique within the skeleton, is how clips (AnimationClipData's channels) and scripts
//     (Animator:GetJointTransform) name it, so a clip plays on any skeleton whose joints carry its channels' names.
//   - Each joint has its rest pose (Translation, Rotation, Scale: the local transform T * R * S relative to its parent, or
//     to model space for a root) and its InverseBind matrix: the inverse of its model transform in the pose the mesh's
//     vertices were bound in, so JointModel * InverseBind is the identity at that pose and a vertex weighted to the joint
//     follows it (a skin matrix, Animation/AnimationPose.h's ComputeSkinMatrices).
//   - The skeleton also holds the nodes between the Animator's entity and the skin's joints that are not themselves joints
//     (static joints that no vertex weights, whose InverseBind is the identity), so model space is always the Animator's
//     entity (GltfImporter.h's version 3 paragraph).
//   - RootMotionJoint names the joint whose planar motion is root motion (Animation/RootMotion.h): the skin's first root
//     joint (the first of the glTF skin's joints whose parent is not one of them), which follows the static joints above
//     it, so a rig exported under an intermediate node ("Character" -> "Rig" -> "Hips") moves its hips, not that node.
//
// Frozen by the M23 contract C23.3; stream A implements the payload, the validation and the lookup.

namespace Engine {

	// The limits ValidateSkeletonData enforces, so a skeleton that external input controls cannot exhaust memory: the joints
	// a mesh's skin can address (MeshData.h) and the bytes of a joint's name.
	inline constexpr uint32_t MaxSkeletonJoints = MaxMeshSkinJoints;
	inline constexpr uint32_t MaxSkeletonJointNameBytes = 256;
	// A root joint's Parent.
	inline constexpr int32_t SkeletonRootParent = -1;

	// One joint (see the file comment).
	struct SkeletonJoint
	{
		std::string Name{}; // 1 to MaxSkeletonJointNameBytes bytes of UTF-8, unique within the skeleton
		int32_t Parent = SkeletonRootParent;
		glm::vec3 Translation{ 0.0f };
		glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f }; // unit length
		glm::vec3 Scale{ 1.0f };
		glm::mat4 InverseBind{ 1.0f }; // affine: its fourth row is (0, 0, 0, 1)

		bool operator==(const SkeletonJoint&) const = default;
	};

	// A loaded skeleton (AssetType::Skeleton). Plain data; immutable once loaded (AssetRef<SkeletonData>).
	struct SkeletonData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Skeleton;
		// The cooked payload layout below (CookedHeader::FormatVersion; ADR 0028's phase-2 ledger: a new format at version 1).
		static constexpr uint16_t FormatVersion = 1;

		SkeletonData()
			: Asset(StaticType)
		{
		}

		std::vector<SkeletonJoint> Joints{};
		// The root-motion joint (see the file comment): an index below the joint count, 0 when no static joint precedes the
		// skin's root.
		uint32_t RootMotionJoint = 0;
	};

	// The invariants every producer guarantees and every reader re-checks on untrusted data: 1 to MaxSkeletonJoints joints;
	// each name valid UTF-8 of 1 to MaxSkeletonJointNameBytes bytes and unique; each Parent SkeletonRootParent or below the
	// joint's own index; every float finite; each Rotation of unit length within 1e-3; no Scale component zero; each
	// InverseBind affine; RootMotionJoint below the joint count. Pure. Errors: Validation naming the first violation and its
	// joint ("joint 4 'Hand.L' has parent 7, which does not precede it").
	[[nodiscard]] Status ValidateSkeletonData(const SkeletonData& skeleton);

	// The index of the joint named exactly `name` (case-sensitive), or nullopt. Pure; linear in the joint count.
	[[nodiscard]] std::optional<uint32_t> FindSkeletonJoint(const SkeletonData& skeleton, std::string_view name);

	// The cooked skeleton payload (FormatVersion 1), little-endian, a string as a uint32 byte count then its UTF-8 bytes:
	//     uint32 JointCount; uint32 RootMotionJoint;
	//     JointCount x { string Name; int32 Parent; f32x3 Translation; f32x4 Rotation (x, y, z, w); f32x3 Scale;
	//                    f32x16 InverseBind (column-major) }
	// Joints in the skeleton's order. Asserts ValidateSkeletonData. Pure; identical skeletons give identical bytes in every
	// configuration (no padding, floats as their bits).
	[[nodiscard]] Buffer SerializeSkeletonPayload(const SkeletonData& skeleton);

	// Reads a payload written by SerializeSkeletonPayload, checking every count and length against its limit and against
	// the bytes left before allocating, then ValidateSkeletonData. Never asserts on data (fuzzed: "SkeletonData: 10,000
	// seeded mutations of a payload never crash", the fuzz target SkeletonData). Errors: Parse for truncation, a count above
	// its limit or trailing bytes; Validation from ValidateSkeletonData.
	[[nodiscard]] Result<SkeletonData> DeserializeSkeletonPayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload, §6.8) of `skeleton`, produced by a source of version
	// `importerVersion`. Asserts ValidateSkeletonData (through SerializeSkeletonPayload).
	[[nodiscard]] Buffer CookSkeleton(const SkeletonData& skeleton, uint32_t importerVersion);

	// The skeleton of a cooked artifact (both asset managers' loader for AssetType::Skeleton, RegisterBuiltinLoaders):
	// ReadCookedArtifact with AssetType::Skeleton and FormatVersion, then the payload. Errors: as ReadCookedArtifact and
	// DeserializeSkeletonPayload.
	[[nodiscard]] Result<AssetRef<SkeletonData>> LoadCookedSkeleton(std::span<const std::byte> cooked);

}
