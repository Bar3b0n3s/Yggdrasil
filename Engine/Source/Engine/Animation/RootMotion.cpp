#include "EnginePCH.h"
#include "Engine/Animation/RootMotion.h"

#include "Engine/Animation/Private/ChannelSampling.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Root motion (RootMotion.h). Arithmetic and sqrt only (AnimationSampling.h's determinism paragraph).

namespace Engine {

	namespace Utils {

		// The joints from a root down to the root-motion joint, parents first, with the channels of `clip` that key each.
		struct RootMotionChain
		{
			struct Link
			{
				uint32_t Joint = 0;
				const AnimationChannel* Translation = nullptr;
				const AnimationChannel* Rotation = nullptr;
				const AnimationChannel* Scale = nullptr;
			};

			std::vector<Link> Links{}; // empty when the skeleton has no root-motion joint

			[[nodiscard]] const Link& GetRootMotionLink() const { return Links.back(); }
			[[nodiscard]] bool KeysTranslation() const { return GetRootMotionLink().Translation != nullptr; }
			[[nodiscard]] bool KeysRotation() const { return GetRootMotionLink().Rotation != nullptr; }
		};

		// The chain of `skeleton`'s root-motion joint, or no links when the skeleton has no joint or its root-motion joint is
		// out of range. A parent that does not precede its joint ends the chain there (a skeleton built in memory against
		// the format's rule).
		static RootMotionChain FindRootMotionChain(const AnimationClipData& clip, const SkeletonData& skeleton)
		{
			RootMotionChain chain;
			if (skeleton.RootMotionJoint >= skeleton.Joints.size())
				return chain;
			uint32_t joint = skeleton.RootMotionJoint;
			while (true)
			{
				chain.Links.push_back({ .Joint = joint });
				const int32_t parent = skeleton.Joints[joint].Parent;
				if (parent < 0 || static_cast<uint32_t>(parent) >= joint)
					break;
				joint = static_cast<uint32_t>(parent);
			}
			std::ranges::reverse(chain.Links);
			for (const AnimationChannel& channel : clip.Channels)
			{
				if (channel.Path == AnimationPath::Weights || !IsReadableAnimationChannel(channel))
					continue;
				for (RootMotionChain::Link& link : chain.Links)
				{
					if (skeleton.Joints[link.Joint].Name != channel.Target)
						continue;
					const AnimationChannel** slot = &link.Scale;
					if (channel.Path == AnimationPath::Translation)
						slot = &link.Translation;
					else if (channel.Path == AnimationPath::Rotation)
						slot = &link.Rotation;
					if (*slot == nullptr)
						*slot = &channel;
				}
			}
			return chain;
		}

		// A chain joint's local transform at `time`: its rest pose with the paths the clip keys sampled.
		static JointTransform SampleLink(const RootMotionChain::Link& link, const SkeletonData& skeleton, float time)
		{
			const SkeletonJoint& joint = skeleton.Joints[link.Joint];
			JointTransform transform{ .Translation = joint.Translation, .Rotation = joint.Rotation, .Scale = joint.Scale };
			if (link.Translation != nullptr)
				transform.Translation = SampleAnimationVector(*link.Translation, time);
			if (link.Rotation != nullptr)
				transform.Rotation = SampleAnimationRotation(*link.Rotation, time);
			if (link.Scale != nullptr)
				transform.Scale = SampleAnimationVector(*link.Scale, time);
			return transform;
		}

		// The root-motion joint's model translation and rotation (the chain's matrices and rotations composed parents first).
		struct RootModel
		{
			glm::vec3 Translation{ 0.0f };
			glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
		};

		static RootModel SampleRootModel(const RootMotionChain& chain, const SkeletonData& skeleton, float time)
		{
			glm::mat4 model(1.0f);
			glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
			for (size_t index = 0; index < chain.Links.size(); ++index)
			{
				const JointTransform local = SampleLink(chain.Links[index], skeleton, time);
				model = index == 0 ? ComposeJointTransform(local) : model * ComposeJointTransform(local);
				rotation = index == 0 ? local.Rotation : rotation * local.Rotation;
			}
			return { .Translation = glm::vec3(model[3]), .Rotation = NormalizeAnimationRotation(rotation) };
		}

		// The twist of `rotation` about +Y (the swing-twist decomposition): normalize((0, y, 0, w)), the identity when y and w
		// are both 0.
		static glm::quat TwistAboutY(const glm::quat& rotation)
		{
			return NormalizeAnimationRotation(glm::quat(rotation.w, 0.0f, rotation.y, 0.0f));
		}

		// The planar part of a model translation: its X and Z.
		static glm::vec3 Planar(const glm::vec3& translation)
		{
			return glm::vec3(translation.x, 0.0f, translation.z);
		}

		// The rotation from `from` to `to` (both unit twists), the later on the left: to * inverse(from).
		static glm::quat TwistDelta(const glm::quat& from, const glm::quat& to)
		{
			return to * glm::conjugate(from);
		}

		// Whether the affine `matrix` (a chain's composed T * R * S) can be inverted: the determinant of its upper 3 x 3 is a
		// finite number other than 0. Arithmetic only, so every configuration decides alike.
		static bool IsInvertibleAffine(const glm::mat4& matrix)
		{
			const float determinant = glm::determinant(glm::mat3(matrix));
			return std::isfinite(determinant) && determinant != 0.0f;
		}

	}

	RootMotionDelta ComputeRootMotion(const AnimationClipData& clip, const SkeletonData& skeleton, const AnimationTimeStep& step)
	{
		const Utils::RootMotionChain chain = Utils::FindRootMotionChain(clip, skeleton);
		if (chain.Links.empty() || (!chain.KeysTranslation() && !chain.KeysRotation()))
			return {};
		const Utils::RootModel previous = Utils::SampleRootModel(chain, skeleton, step.PreviousTime);
		const Utils::RootModel current = Utils::SampleRootModel(chain, skeleton, step.Time);
		glm::vec3 translation(0.0f);
		glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
		if (step.Wraps == 0)
		{
			translation = Utils::Planar(current.Translation) - Utils::Planar(previous.Translation);
			rotation = Utils::TwistDelta(Utils::TwistAboutY(previous.Rotation), Utils::TwistAboutY(current.Rotation));
		}
		else
		{
			// To the clip's end, every whole cycle between, then from its start.
			const Utils::RootModel start = Utils::SampleRootModel(chain, skeleton, 0.0f);
			const Utils::RootModel end = Utils::SampleRootModel(chain, skeleton, std::max(clip.Duration, 0.0f));
			const glm::vec3 cycleTranslation = Utils::Planar(end.Translation) - Utils::Planar(start.Translation);
			const glm::quat startTwist = Utils::TwistAboutY(start.Rotation);
			const glm::quat endTwist = Utils::TwistAboutY(end.Rotation);
			const glm::quat cycleRotation = Utils::TwistDelta(startTwist, endTwist);
			translation = Utils::Planar(end.Translation) - Utils::Planar(previous.Translation);
			rotation = Utils::TwistDelta(Utils::TwistAboutY(previous.Rotation), endTwist);
			const uint32_t cycles = std::min(step.Wraps, MaxAnimationWrapsPerStep);
			for (uint32_t cycle = 1; cycle < cycles; ++cycle)
			{
				translation += cycleTranslation;
				rotation = cycleRotation * rotation;
			}
			translation += Utils::Planar(current.Translation) - Utils::Planar(start.Translation);
			rotation = Utils::TwistDelta(startTwist, Utils::TwistAboutY(current.Rotation)) * rotation;
		}
		RootMotionDelta delta;
		if (chain.KeysTranslation())
			delta.Translation = glm::vec3(translation.x, 0.0f, translation.z);
		if (chain.KeysRotation())
		{
			// One representative of the rotation, w >= 0, so blends and scripts see the same quaternion for the same turn.
			const glm::quat normalized = Utils::NormalizeAnimationRotation(rotation);
			delta.Rotation = normalized.w < 0.0f ? -normalized : normalized;
		}
		return delta;
	}

	void HoldRootMotion(const AnimationClipData& clip, const SkeletonData& skeleton, AnimationPose& layerPose)
	{
		const Utils::RootMotionChain chain = Utils::FindRootMotionChain(clip, skeleton);
		if (chain.Links.empty() || (!chain.KeysTranslation() && !chain.KeysRotation()))
			return;
		const uint32_t rootMotionJoint = chain.GetRootMotionLink().Joint;
		for (const Utils::RootMotionChain::Link& link : chain.Links)
			if (link.Joint >= layerPose.Joints.size())
				return;
		const Utils::RootModel start = Utils::SampleRootModel(chain, skeleton, 0.0f);
		// The parent's model transform and rotation at the pose (the identity for a root).
		glm::mat4 parentModel(1.0f);
		glm::quat parentRotation(1.0f, 0.0f, 0.0f, 0.0f);
		for (size_t index = 0; index + 1 < chain.Links.size(); ++index)
		{
			const JointTransform& local = layerPose.Joints[chain.Links[index].Joint];
			parentModel = index == 0 ? ComposeJointTransform(local) : parentModel * ComposeJointTransform(local);
			parentRotation = index == 0 ? local.Rotation : parentRotation * local.Rotation;
		}
		const bool root = chain.Links.size() == 1;
		JointTransform& joint = layerPose.Joints[rootMotionJoint];
		if (chain.KeysTranslation())
		{
			if (root)
			{
				joint.Translation.x = start.Translation.x;
				joint.Translation.z = start.Translation.z;
			}
			else if (Utils::IsInvertibleAffine(parentModel))
			{
				// Through the parent's model transform; a parent a clip collapses (a scale of 0 on the chain) has no inverse, so
				// nothing can be held through it and the sampled translation stays.
				glm::vec4 model = parentModel * glm::vec4(joint.Translation, 1.0f);
				model.x = start.Translation.x;
				model.z = start.Translation.z;
				const glm::vec3 held(glm::inverse(parentModel) * model);
				if (std::isfinite(held.x) && std::isfinite(held.y) && std::isfinite(held.z))
					joint.Translation = held;
			}
		}
		if (chain.KeysRotation())
		{
			const glm::quat model = root ? joint.Rotation : Utils::NormalizeAnimationRotation(parentRotation * joint.Rotation);
			const glm::quat held = Utils::TwistAboutY(start.Rotation) * glm::conjugate(Utils::TwistAboutY(model)) * model;
			joint.Rotation = Utils::NormalizeAnimationRotation(root ? held : glm::conjugate(Utils::NormalizeAnimationRotation(parentRotation)) * held);
		}
	}

	void BlendRootMotion(RootMotionDelta& result, const RootMotionDelta& layer, uint8_t keyed, float weight)
	{
		const float clamped = std::clamp(weight, 0.0f, 1.0f);
		if (!(clamped > 0.0f))
			return;
		const bool copy = clamped >= 1.0f;
		if ((keyed & AnimationPoseTranslationBit) != 0)
			result.Translation = copy ? layer.Translation : result.Translation + (layer.Translation - result.Translation) * clamped;
		if ((keyed & AnimationPoseRotationBit) != 0)
			result.Rotation = copy ? layer.Rotation : Utils::BlendAnimationRotation(result.Rotation, layer.Rotation, clamped);
	}

}
