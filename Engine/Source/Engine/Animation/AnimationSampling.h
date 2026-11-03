#pragma once

#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <vector>

// The Animation module (M23, C23.3; Docs/Decisions/0034-m23-contract.md decisions 22 and 26): skeletal and morph animation's
// runtime over the Asset module's SkeletonData and AnimationClipData, ECS-agnostic like Physics and Audio. Scene's
// AnimationSystem (the AnimationSystem phase of the fixed step) and the editor's preview drive it; scripts reach it through
// the Animator proxy. Layer 3 beside Scene, which includes it (Scripts/ModuleRules.json).
//
// Determinism (§1.3, §4.12): animation is sampled in the fixed step, on the simulation path, so this module uses only
// IEEE-exact operations, sqrt and Core/DetMath (Lint's banned-API step covers it, Banned.SimulationPathModules): never a CRT
// transcendental, and never glm::slerp, glm::mix of quaternions, glm::angleAxis or glm::eulerAngles, which call them. The
// same clip, skeleton, times and weights give bit-identical poses in Debug, Release and Dist ("AnimationPose: a sampled pose
// hashes to its committed digest in every configuration").
//
// This header: a joint's local transform, the quaternion interpolation, the advance of a layer's time and the events it
// passes. Frozen by the M23 contract C23.3; stream B implements it.

namespace Engine {

	// A joint's local transform: the matrix T * R * S (translate, rotate, scale), relative to its parent joint, or to the
	// Animator entity's space for a root joint (SkeletonData.h).
	struct JointTransform
	{
		glm::vec3 Translation{ 0.0f };
		glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale{ 1.0f };

		bool operator==(const JointTransform&) const = default;
	};

	// T * R * S of `transform` (glm::mat4_cast of the rotation, which is arithmetic only). Pure.
	[[nodiscard]] glm::mat4 ComposeJointTransform(const JointTransform& transform);

	// Spherical linear interpolation from `from` to `to` (both unit length) at `t` in [0, 1], along the shorter arc (`to`
	// negated when the dot product is negative), through DetMath's ACos and Sin; normalized linear interpolation when the
	// two are within 1e-4 of parallel (the angle's sine is then too small to divide by). t 0 gives `from` and t 1 the
	// shorter-arc `to` exactly. The result is normalized. Pure; bit-identical in every configuration.
	[[nodiscard]] glm::quat SlerpQuaternion(const glm::quat& from, const glm::quat& to, float t);

	// The most ends of its clip a looping layer counts in one step (AnimationTimeStep::Wraps), so the cycles a step fires
	// events and root motion for stay bounded whatever the clip's duration and the layer's speed (a clip is at least
	// MinAnimationClipDuration long, AnimationClipData.h, and Speed at most MaxAnimatorSpeed, but a fixed step of 1/60 s at
	// that speed still passes about 1,700 ends of the shortest clip). Past it the layer's Time is still exact; only the events
	// and the root motion of the cycles beyond are dropped, which a clip that short at that speed cannot show anyway
	// (decision 26).
	inline constexpr uint32_t MaxAnimationWrapsPerStep = 8;

	// One fixed step of a layer's time (AdvanceAnimationTime).
	struct AnimationTimeStep
	{
		float PreviousTime = 0.0f; // the layer's time before the step, as it was clamped or wrapped into [0, Duration]
		float Time = 0.0f;         // after the step, in [0, Duration]
		uint32_t Wraps = 0;        // how often a looping layer passed its end, at most MaxAnimationWrapsPerStep
		bool Ended = false;        // a layer without Loop reached its end in this step (it holds there afterwards)

		bool operator==(const AnimationTimeStep&) const = default;
	};

	// Advances a layer's `time` by `delta` (the fixed delta times the layer's Speed, >= 0, finite) over a clip of `duration`
	// seconds (>= 0). The time first comes into [0, duration]: a looping layer's by subtracting whole durations (computed in
	// double: time - duration * floor(time / duration)), another's clamped to its end. Then: with `loop`, the advanced time
	// wraps the same way, Wraps counting the ends passed (floor of the advanced time over the duration, in double), saturated
	// at MaxAnimationWrapsPerStep before its conversion, so no count, however large, converts out of range; without it, it
	// stops at `duration`, and Ended is set in the step that reaches it from before it. A clip of duration 0 keeps time 0 and
	// never wraps or ends. Pure.
	[[nodiscard]] AnimationTimeStep AdvanceAnimationTime(float time, float delta, float duration, bool loop);

	// The events `step` passes, appended to `events` as indices into clip.Events in the order they fire: an event at e
	// fires when PreviousTime <= e < Time. A looping step that passes its clip's end fires those in [PreviousTime, Duration]
	// (an event at exactly Duration included, as an Ended step fires it), then every event of the clip once for each of the
	// Wraps - 1 whole cycles in between, then those in [0, Time), so an event at Duration and one at 0 both fire at each end
	// passed, the one at Duration first. An Ended step fires those in [PreviousTime, Duration]. Events of equal time fire in
	// their order in the clip. A step of zero delta fires nothing. At most (Wraps + 1) times the clip's events. Pure.
	void CollectAnimationEvents(const AnimationClipData& clip, const AnimationTimeStep& step, std::vector<uint32_t>& events);

}
