#include "EnginePCH.h"
#include "Engine/Asset/SkeletonData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string_view>
#include <utility>

// The skeleton's cooked format (SkeletonData.h; Docs/Decisions/0034-m23-contract.md decision 24): the artifact wrappers are
// the contract's, the payload, the validation and the lookup stream A's.

namespace Engine {

	namespace Utils {

		// How far a joint's rest rotation may be from unit length (ValidateSkeletonData).
		static constexpr double SkeletonUnitRotationTolerance = 1e-3;

		// The fewest bytes one joint record takes in a payload: an empty name's length word, the parent, the translation,
		// rotation, scale and inverse bind (4 + 4 + 12 + 16 + 12 + 64); for the size check before anything is allocated.
		static constexpr uint64_t MinJointPayloadBytes = 4 + 4 + 12 + 16 + 12 + 64;

		[[nodiscard]] static bool IsFinite(std::span<const float> values)
		{
			return std::ranges::all_of(values, [](float value)
			{
				return std::isfinite(value);
			});
		}

		// The joint's floats in payload order, for the finiteness check.
		[[nodiscard]] static std::array<float, 26> GetJointFloats(const SkeletonJoint& joint)
		{
			std::array<float, 26> values{};
			size_t next = 0;
			for (glm::length_t axis = 0; axis < 3; ++axis)
				values[next++] = joint.Translation[axis];
			for (const float component : { joint.Rotation.x, joint.Rotation.y, joint.Rotation.z, joint.Rotation.w })
				values[next++] = component;
			for (glm::length_t axis = 0; axis < 3; ++axis)
				values[next++] = joint.Scale[axis];
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
					values[next++] = joint.InverseBind[column][row];
			}
			return values;
		}

		static void WriteJoint(BinaryWriter& writer, const SkeletonJoint& joint)
		{
			writer.WriteString(joint.Name);
			writer.WriteI32(joint.Parent);
			for (glm::length_t axis = 0; axis < 3; ++axis)
				writer.WriteF32(joint.Translation[axis]);
			writer.WriteF32(joint.Rotation.x);
			writer.WriteF32(joint.Rotation.y);
			writer.WriteF32(joint.Rotation.z);
			writer.WriteF32(joint.Rotation.w);
			for (glm::length_t axis = 0; axis < 3; ++axis)
				writer.WriteF32(joint.Scale[axis]);
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
					writer.WriteF32(joint.InverseBind[column][row]);
			}
		}

		[[nodiscard]] static Status ReadVector(BinaryReader& reader, glm::vec3& value)
		{
			for (glm::length_t axis = 0; axis < 3; ++axis)
			{
				ENGINE_TRY_ASSIGN(value[axis], reader.ReadF32());
			}
			return {};
		}

		[[nodiscard]] static Result<SkeletonJoint> ReadJoint(BinaryReader& reader, uint32_t index)
		{
			SkeletonJoint joint;
			ENGINE_TRY_ASSIGN(joint.Name, WithContext(reader.ReadString(), std::format("in the name of joint {}", index)));
			ENGINE_TRY_ASSIGN(joint.Parent, reader.ReadI32());
			ENGINE_TRY(ReadVector(reader, joint.Translation));
			ENGINE_TRY_ASSIGN(joint.Rotation.x, reader.ReadF32());
			ENGINE_TRY_ASSIGN(joint.Rotation.y, reader.ReadF32());
			ENGINE_TRY_ASSIGN(joint.Rotation.z, reader.ReadF32());
			ENGINE_TRY_ASSIGN(joint.Rotation.w, reader.ReadF32());
			ENGINE_TRY(ReadVector(reader, joint.Scale));
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					ENGINE_TRY_ASSIGN(joint.InverseBind[column][row], reader.ReadF32());
				}
			}
			return joint;
		}

	}

	Status ValidateSkeletonData(const SkeletonData& skeleton)
	{
		if (skeleton.Joints.empty())
			return MakeError(ErrorCode::Validation, "the skeleton has no joints");
		if (skeleton.Joints.size() > MaxSkeletonJoints)
		{
			return MakeError(ErrorCode::Validation, "the skeleton has {} joints, more than the {} a skeleton may have",
				skeleton.Joints.size(), MaxSkeletonJoints);
		}

		// Names sorted with their joint index, so a duplicate is found in O(n log n) and reported at its later joint.
		std::vector<std::pair<std::string_view, size_t>> names;
		names.reserve(skeleton.Joints.size());
		for (size_t index = 0; index < skeleton.Joints.size(); ++index)
		{
			const SkeletonJoint& joint = skeleton.Joints[index];
			if (joint.Name.empty() || joint.Name.size() > MaxSkeletonJointNameBytes || !IsValidUtf8(joint.Name))
			{
				return MakeError(ErrorCode::Validation, "joint {} has a name that is not 1 to {} bytes of valid UTF-8", index,
					MaxSkeletonJointNameBytes);
			}
			if (joint.Parent != SkeletonRootParent && (joint.Parent < 0 || static_cast<size_t>(joint.Parent) >= index))
			{
				return MakeError(ErrorCode::Validation, "joint {} '{}' has parent {}, which does not precede it", index, joint.Name, joint.Parent);
			}
			if (!Utils::IsFinite(Utils::GetJointFloats(joint)))
				return MakeError(ErrorCode::Validation, "joint {} '{}' has a value that is not a finite number", index, joint.Name);
			const double x = joint.Rotation.x;
			const double y = joint.Rotation.y;
			const double z = joint.Rotation.z;
			const double w = joint.Rotation.w;
			if (std::fabs(std::sqrt(x * x + y * y + z * z + w * w) - 1.0) > Utils::SkeletonUnitRotationTolerance)
				return MakeError(ErrorCode::Validation, "joint {} '{}' has a rest rotation that is not unit length", index, joint.Name);
			if (joint.Scale.x == 0.0f || joint.Scale.y == 0.0f || joint.Scale.z == 0.0f)
				return MakeError(ErrorCode::Validation, "joint {} '{}' has a rest scale with a zero component", index, joint.Name);
			const glm::mat4& inverseBind = joint.InverseBind;
			if (inverseBind[0][3] != 0.0f || inverseBind[1][3] != 0.0f || inverseBind[2][3] != 0.0f || inverseBind[3][3] != 1.0f)
			{
				return MakeError(ErrorCode::Validation, "joint {} '{}' has an inverse bind matrix whose fourth row is not (0, 0, 0, 1)",
					index, joint.Name);
			}
			names.emplace_back(joint.Name, index);
		}
		std::ranges::sort(names);
		for (size_t position = 1; position < names.size(); ++position)
		{
			if (names[position].first == names[position - 1].first)
			{
				return MakeError(ErrorCode::Validation, "joint {} is named '{}', as joint {} is; joint names are unique within a skeleton", names[position].second,
					names[position].first, names[position - 1].second);
			}
		}

		if (skeleton.RootMotionJoint >= skeleton.Joints.size())
		{
			return MakeError(ErrorCode::Validation, "the root-motion joint is {}, beyond the {} joints", skeleton.RootMotionJoint,
				skeleton.Joints.size());
		}
		return {};
	}

	std::optional<uint32_t> FindSkeletonJoint(const SkeletonData& skeleton, std::string_view name)
	{
		for (size_t index = 0; index < skeleton.Joints.size(); ++index)
		{
			if (skeleton.Joints[index].Name == name)
				return static_cast<uint32_t>(index);
		}
		return std::nullopt;
	}

	Buffer SerializeSkeletonPayload(const SkeletonData& skeleton)
	{
		ENGINE_CORE_ASSERT(ValidateSkeletonData(skeleton).has_value(), "SerializeSkeletonPayload needs a skeleton that passes ValidateSkeletonData");

		BinaryWriter writer;
		writer.WriteU32(static_cast<uint32_t>(skeleton.Joints.size()));
		writer.WriteU32(skeleton.RootMotionJoint);
		for (const SkeletonJoint& joint : skeleton.Joints)
			Utils::WriteJoint(writer, joint);
		return writer.TakeBuffer();
	}

	Result<SkeletonData> DeserializeSkeletonPayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		ENGINE_TRY_ASSIGN(const uint32_t jointCount, reader.ReadU32());
		ENGINE_TRY_ASSIGN(const uint32_t rootMotionJoint, reader.ReadU32());
		if (jointCount > MaxSkeletonJoints)
		{
			return MakeError(ErrorCode::Parse, "skeleton payload declares {} joints, more than the {} a skeleton may have", jointCount,
				MaxSkeletonJoints);
		}
		if (jointCount * Utils::MinJointPayloadBytes > reader.GetRemaining())
		{
			return MakeError(ErrorCode::Parse, "skeleton payload declares {} joints, more than the {} remaining bytes at offset {} can hold", jointCount,
				reader.GetRemaining(), reader.GetPosition());
		}

		SkeletonData skeleton;
		skeleton.RootMotionJoint = rootMotionJoint;
		skeleton.Joints.reserve(jointCount);
		for (uint32_t index = 0; index < jointCount; ++index)
		{
			ENGINE_TRY_ASSIGN(SkeletonJoint joint, Utils::ReadJoint(reader, index));
			skeleton.Joints.push_back(std::move(joint));
		}
		if (!reader.IsAtEnd())
			return MakeError(ErrorCode::Parse, "skeleton payload has {} trailing bytes after offset {}", reader.GetRemaining(), reader.GetPosition());
		ENGINE_TRY(WithContext(ValidateSkeletonData(skeleton), "in a cooked skeleton payload"));
		return skeleton;
	}

	Buffer CookSkeleton(const SkeletonData& skeleton, uint32_t importerVersion)
	{
		const Buffer payload = SerializeSkeletonPayload(skeleton);
		return WriteCookedArtifact(AssetType::Skeleton, SkeletonData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<SkeletonData>> LoadCookedSkeleton(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::Skeleton, SkeletonData::FormatVersion));
		ENGINE_TRY_ASSIGN(SkeletonData skeleton, DeserializeSkeletonPayload(artifact.Payload));
		return AssetRef<SkeletonData>(CreateRef<SkeletonData>(std::move(skeleton)));
	}

}
