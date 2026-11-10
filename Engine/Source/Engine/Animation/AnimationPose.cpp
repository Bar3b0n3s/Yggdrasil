#include "EnginePCH.h"
#include "Engine/Animation/AnimationPose.h"

#include "Engine/Animation/Private/ChannelSampling.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// Poses, layer blending, joint and skin matrices and the pose digest (AnimationPose.h). IEEE-exact operations, sqrt and
// DetMath only (AnimationSampling.h's determinism paragraph).

namespace Engine {

	namespace Utils {

		// The skeleton's joints by name, for the lookup of a clip's many channels: sorted by name, then index, so the first
		// joint of a name wins (FindSkeletonJoint's exact, case-sensitive match; a valid skeleton's names are unique).
		class JointNameIndex
		{
		public:
			explicit JointNameIndex(const SkeletonData& skeleton)
			{
				m_Entries.reserve(skeleton.Joints.size());
				for (size_t index = 0; index < skeleton.Joints.size(); ++index)
					m_Entries.emplace_back(skeleton.Joints[index].Name, static_cast<uint32_t>(index));
				std::ranges::sort(m_Entries);
			}

			[[nodiscard]] std::optional<uint32_t> Find(std::string_view name) const
			{
				const auto found = std::ranges::lower_bound(m_Entries, name, {}, &Entry::first);
				if (found == m_Entries.end() || found->first != name)
					return std::nullopt;
				return found->second;
			}
		private:
			using Entry = std::pair<std::string_view, uint32_t>;
		private:
			std::vector<Entry> m_Entries;
		};

		// The 4 little-endian bytes of `bits`.
		static void HashBits(XXH64Hasher& hasher, uint32_t bits)
		{
			const std::array<std::byte, 4> bytes{ static_cast<std::byte>(bits & 0xffU), static_cast<std::byte>((bits >> 8) & 0xffU),
				static_cast<std::byte>((bits >> 16) & 0xffU), static_cast<std::byte>((bits >> 24) & 0xffU) };
			hasher.Update(bytes);
		}

	}

	AnimationPose MakeRestPose(const SkeletonData& skeleton)
	{
		AnimationPose pose;
		pose.Joints.reserve(skeleton.Joints.size());
		for (const SkeletonJoint& joint : skeleton.Joints)
			pose.Joints.push_back(JointTransform{ .Translation = joint.Translation, .Rotation = joint.Rotation, .Scale = joint.Scale });
		pose.Keyed.assign(skeleton.Joints.size(), 0);
		return pose;
	}

	void SampleAnimationClip(const AnimationClipData& clip, const SkeletonData& skeleton, float time, AnimationPose& pose)
	{
		const float clamped = std::clamp(time, 0.0f, std::max(clip.Duration, 0.0f));
		if (pose.Keyed.size() < pose.Joints.size())
			pose.Keyed.resize(pose.Joints.size(), 0);
		std::optional<Utils::JointNameIndex> joints;
		for (const AnimationChannel& channel : clip.Channels)
		{
			if (!Utils::IsReadableAnimationChannel(channel))
				continue;
			if (channel.Path == AnimationPath::Weights)
			{
				std::vector<float> weights = Utils::SampleAnimationWeights(channel, clamped);
				const auto existing = std::ranges::find(pose.Morphs, channel.Target, &AnimationMorphWeights::Target);
				if (existing != pose.Morphs.end())
					existing->Weights = std::move(weights);
				else
					pose.Morphs.push_back(AnimationMorphWeights{ .Target = channel.Target, .Weights = std::move(weights) });
				continue;
			}
			if (!joints.has_value())
				joints.emplace(skeleton);
			const std::optional<uint32_t> joint = joints->Find(channel.Target);
			if (!joint.has_value() || *joint >= pose.Joints.size())
				continue;
			JointTransform& transform = pose.Joints[*joint];
			switch (channel.Path)
			{
				case AnimationPath::Translation:
					transform.Translation = Utils::SampleAnimationVector(channel, clamped);
					pose.Keyed[*joint] |= AnimationPoseTranslationBit;
					break;
				case AnimationPath::Rotation:
					transform.Rotation = Utils::SampleAnimationRotation(channel, clamped);
					pose.Keyed[*joint] |= AnimationPoseRotationBit;
					break;
				case AnimationPath::Scale:
					transform.Scale = Utils::SampleAnimationVector(channel, clamped);
					pose.Keyed[*joint] |= AnimationPoseScaleBit;
					break;
				case AnimationPath::Weights: break;
			}
		}
	}

	void BlendAnimationPose(AnimationPose& result, const AnimationPose& layer, float weight)
	{
		const float clamped = std::clamp(weight, 0.0f, 1.0f);
		if (!(clamped > 0.0f))
			return;
		const bool copy = clamped >= 1.0f;
		if (result.Keyed.size() < result.Joints.size())
			result.Keyed.resize(result.Joints.size(), 0);
		const size_t joints = std::min({ result.Joints.size(), layer.Joints.size(), layer.Keyed.size() });
		for (size_t index = 0; index < joints; ++index)
		{
			const uint8_t keyed = layer.Keyed[index];
			if (keyed == 0)
				continue;
			JointTransform& target = result.Joints[index];
			const JointTransform& source = layer.Joints[index];
			if ((keyed & AnimationPoseTranslationBit) != 0)
				target.Translation = copy ? source.Translation : target.Translation + (source.Translation - target.Translation) * clamped;
			if ((keyed & AnimationPoseRotationBit) != 0)
				target.Rotation = copy ? source.Rotation : Utils::BlendAnimationRotation(target.Rotation, source.Rotation, clamped);
			if ((keyed & AnimationPoseScaleBit) != 0)
				target.Scale = copy ? source.Scale : target.Scale + (source.Scale - target.Scale) * clamped;
			result.Keyed[index] |= keyed;
		}
		for (const AnimationMorphWeights& morph : layer.Morphs)
		{
			auto base = std::ranges::find(result.Morphs, morph.Target, &AnimationMorphWeights::Target);
			if (base == result.Morphs.end())
			{
				result.Morphs.push_back(AnimationMorphWeights{ .Target = morph.Target });
				base = result.Morphs.end() - 1;
			}
			// A base shorter than the layer's list is padded with zeros; its weights past the layer's keep their values, since
			// the layer keys only its own.
			if (base->Weights.size() < morph.Weights.size())
				base->Weights.resize(morph.Weights.size(), 0.0f);
			for (size_t index = 0; index < morph.Weights.size(); ++index)
			{
				float& value = base->Weights[index];
				value = copy ? morph.Weights[index] : value + (morph.Weights[index] - value) * clamped;
			}
		}
	}

	std::vector<glm::mat4> ComputeJointModelTransforms(const SkeletonData& skeleton, const AnimationPose& pose)
	{
		std::vector<glm::mat4> model;
		model.reserve(skeleton.Joints.size());
		for (size_t index = 0; index < skeleton.Joints.size(); ++index)
		{
			const SkeletonJoint& joint = skeleton.Joints[index];
			const glm::mat4 local = ComposeJointTransform(index < pose.Joints.size()
					? pose.Joints[index]
					: JointTransform{ .Translation = joint.Translation, .Rotation = joint.Rotation, .Scale = joint.Scale });
			// A parent that does not precede its joint (a skeleton built in memory against the format's rule) is ignored.
			const bool hasParent = joint.Parent >= 0 && static_cast<size_t>(joint.Parent) < index;
			model.push_back(hasParent ? model[static_cast<size_t>(joint.Parent)] * local : local);
		}
		return model;
	}

	std::vector<glm::mat4> ComputeSkinMatrices(const SkeletonData& skeleton, std::span<const glm::mat4> jointModel)
	{
		ENGINE_CORE_ASSERT(jointModel.size() == skeleton.Joints.size(), "ComputeSkinMatrices needs one model matrix per joint: {} for {} joints",
			jointModel.size(), skeleton.Joints.size());
		std::vector<glm::mat4> skin;
		skin.reserve(jointModel.size());
		for (size_t index = 0; index < jointModel.size(); ++index)
			skin.push_back(jointModel[index] * skeleton.Joints[index].InverseBind);
		return skin;
	}

	uint64_t ComputeAnimationPoseDigest(std::span<const glm::mat4> jointModel, std::span<const AnimationMorphWeights> morphs)
	{
		XXH64Hasher hasher;
		for (const glm::mat4& matrix : jointModel)
			for (glm::length_t column = 0; column < 4; ++column)
				for (glm::length_t row = 0; row < 4; ++row)
					Utils::HashBits(hasher, std::bit_cast<uint32_t>(matrix[column][row]));
		for (const AnimationMorphWeights& morph : morphs)
		{
			hasher.Update(std::string_view(morph.Target));
			Utils::HashBits(hasher, static_cast<uint32_t>(morph.Weights.size()));
			for (const float weight : morph.Weights)
				Utils::HashBits(hasher, std::bit_cast<uint32_t>(weight));
		}
		return hasher.Digest();
	}

}
