#pragma once

#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

// One channel of an animation clip sampled at a time (AnimationPose.h's SampleAnimationClip rule, glTF 2.0 Appendix C), and
// the rotation helpers the pose and root-motion blends share. IEEE-exact operations, sqrt and DetMath only
// (AnimationSampling.h's determinism paragraph).

namespace Engine {

	namespace Utils {

		// Whether `channel`'s keys can be read: at least one key, and the floats every key needs. A clip that loads was
		// validated (ValidateAnimationClipData); a clip built in memory is checked here, so sampling never reads past it.
		[[nodiscard]] bool IsReadableAnimationChannel(const AnimationChannel& channel);

		// A readable Translation or Scale channel at `time`: before the first key its first value, from the last on its last;
		// Step the earlier key, Linear the straight line, CubicSpline the Hermite spline of the values and the tangents
		// scaled by the key interval.
		[[nodiscard]] glm::vec3 SampleAnimationVector(const AnimationChannel& channel, float time);

		// A readable Rotation channel at `time`, as SampleAnimationVector, Linear by SlerpQuaternion and CubicSpline
		// normalized.
		[[nodiscard]] glm::quat SampleAnimationRotation(const AnimationChannel& channel, float time);

		// A readable Weights channel's WeightCount weights at `time`, as SampleAnimationVector.
		[[nodiscard]] std::vector<float> SampleAnimationWeights(const AnimationChannel& channel, float time);

		// `rotation` scaled to unit length; the identity for a zero rotation (a degenerate spline).
		[[nodiscard]] glm::quat NormalizeAnimationRotation(const glm::quat& rotation);

		// `from` blended towards `to` by `weight` in [0, 1]: normalized linear interpolation along the shorter arc.
		[[nodiscard]] glm::quat BlendAnimationRotation(const glm::quat& from, const glm::quat& to, float weight);

	}

}
