#include "TestsPCH.h"

#include "Engine/Animation/AnimationSampling.h"

#include "Engine/Core/DetMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

// Joint transforms, quaternion interpolation, a layer's time and its events (M23, C23.3; Docs/Decisions/0034-m23-contract.md
// decision 26, stream B). Skipped until stream B implements them.

namespace Engine {

	namespace {

		// A rotation of `degrees` about +Z, built with DetMath (the simulation path's trigonometry).
		glm::quat RotationZ(double degrees)
		{
			const double half = degrees * 3.14159265358979323846 / 360.0;
			return glm::quat(static_cast<float>(DetMath::Cos(half)), 0.0f, 0.0f, static_cast<float>(DetMath::Sin(half)));
		}

		AnimationClipData MakeEventClip()
		{
			AnimationClipData clip;
			clip.Duration = 1.0f;
			clip.Events = { AnimationEvent{ .Name = "Start", .Time = 0.0f }, AnimationEvent{ .Name = "Step", .Time = 0.25f },
				AnimationEvent{ .Name = "StepTwin", .Time = 0.25f }, AnimationEvent{ .Name = "End", .Time = 1.0f } };
			return clip;
		}

	}

	TEST_SUITE("Animation")
	{
		TEST_CASE("AnimationSampling: a joint transform composes translation, then rotation, then scale" * doctest::skip(true))
		{
			const JointTransform transform{ .Translation = { 1.0f, 2.0f, 3.0f }, .Rotation = RotationZ(90.0), .Scale = { 2.0f, 2.0f, 2.0f } };
			const glm::mat4 matrix = ComposeJointTransform(transform);
			// (1, 0, 0) scaled to (2, 0, 0), turned to (0, 2, 0), moved to (1, 4, 3).
			const glm::vec4 point = matrix * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
			CHECK(std::abs(point.x - 1.0f) < 1e-6f);
			CHECK(std::abs(point.y - 4.0f) < 1e-6f);
			CHECK(point.z == 3.0f);
			CHECK(ComposeJointTransform(JointTransform{}) == glm::mat4(1.0f));
		}

		// SlerpQuaternion is the simulation path's slerp (glm::slerp calls the CRT's acos and sin): the ends exactly, the middle
		// of a 90-degree arc at 45 degrees, the shorter arc for a negated target, and normalized linear interpolation for
		// nearly parallel rotations.
		TEST_CASE("AnimationSampling: rotations slerp along the shorter arc through DetMath" * doctest::skip(true))
		{
			const glm::quat from = RotationZ(0.0);
			const glm::quat to = RotationZ(90.0);
			CHECK(SlerpQuaternion(from, to, 0.0f) == from);
			CHECK(SlerpQuaternion(from, to, 1.0f) == to);
			const glm::quat middle = SlerpQuaternion(from, to, 0.5f);
			const glm::quat expected = RotationZ(45.0);
			CHECK(std::abs(middle.w - expected.w) < 1e-6f);
			CHECK(std::abs(middle.z - expected.z) < 1e-6f);
			// The negated target is the same rotation: the shorter arc gives the same middle.
			const glm::quat viaNegated = SlerpQuaternion(from, -to, 0.5f);
			CHECK(std::abs(std::abs(viaNegated.w) - expected.w) < 1e-6f);
			CHECK(std::abs(std::abs(viaNegated.z) - expected.z) < 1e-6f);
			// Unit length, also within 1e-4 of parallel.
			const glm::quat close = SlerpQuaternion(from, RotationZ(0.001), 0.5f);
			CHECK(std::abs(glm::length(close) - 1.0f) < 1e-6f);
		}

		TEST_CASE("AnimationSampling: time advances, wraps a looping clip and holds a clip that ends" * doctest::skip(true))
		{
			// A looping layer wraps, counting the ends it passes.
			const AnimationTimeStep looped = AdvanceAnimationTime(0.75f, 0.5f, 1.0f, true);
			CHECK(looped.PreviousTime == 0.75f);
			CHECK(looped.Time == 0.25f);
			CHECK(looped.Wraps == 1);
			CHECK_FALSE(looped.Ended);
			const AnimationTimeStep twice = AdvanceAnimationTime(0.5f, 2.0f, 1.0f, true);
			CHECK(twice.Time == 0.5f);
			CHECK(twice.Wraps == 2);
			// A time beyond the clip comes into it first: wrapped, or clamped without Loop.
			CHECK(AdvanceAnimationTime(3.25f, 0.0f, 1.0f, true).PreviousTime == 0.25f);
			CHECK(AdvanceAnimationTime(3.25f, 0.0f, 1.0f, false).PreviousTime == 1.0f);
			// Without Loop the clip holds its end, and Ended is set only in the step that reaches it.
			const AnimationTimeStep ending = AdvanceAnimationTime(0.75f, 0.5f, 1.0f, false);
			CHECK(ending.Time == 1.0f);
			CHECK(ending.Ended);
			CHECK(ending.Wraps == 0);
			const AnimationTimeStep held = AdvanceAnimationTime(1.0f, 0.5f, 1.0f, false);
			CHECK(held.Time == 1.0f);
			CHECK_FALSE(held.Ended);
			// A clip of duration 0 keeps time 0.
			const AnimationTimeStep still = AdvanceAnimationTime(0.0f, 0.5f, 0.0f, true);
			CHECK(still.Time == 0.0f);
			CHECK(still.Wraps == 0);
			CHECK_FALSE(still.Ended);
			// A looping clip far shorter than the step counts at most MaxAnimationWrapsPerStep ends, and its time stays exact:
			// the shortest clip at the fastest speed passes about 1,700 ends in a 1/60 s step, and a fuzz-shaped clip of
			// 1e-12 s would pass 10^12, past what a uint32_t holds (the count saturates before its conversion).
			const double fastestDelta = static_cast<double>(100.0f / 60.0f);
			const AnimationTimeStep fastest = AdvanceAnimationTime(0.0f, 100.0f / 60.0f, MinAnimationClipDuration, true);
			CHECK(fastest.Wraps == MaxAnimationWrapsPerStep);
			const double shortest = static_cast<double>(MinAnimationClipDuration);
			CHECK(std::abs(static_cast<double>(fastest.Time) - (fastestDelta - shortest * std::floor(fastestDelta / shortest))) < 1e-6);
			const AnimationTimeStep fuzzed = AdvanceAnimationTime(0.0f, 1.0f, 1.0e-12f, true);
			CHECK(fuzzed.Wraps == MaxAnimationWrapsPerStep);
			CHECK(fuzzed.Time >= 0.0f);
			CHECK(fuzzed.Time <= 1.0e-12f);
			CHECK_FALSE(fuzzed.Ended);
		}

		TEST_CASE("AnimationSampling: events between two times fire once, in time order, across a wrap" * doctest::skip(true))
		{
			const AnimationClipData clip = MakeEventClip();
			std::vector<uint32_t> events;
			// [0, 0.5): Start, Step, StepTwin (equal times in their clip order).
			CollectAnimationEvents(clip, AdvanceAnimationTime(0.0f, 0.5f, 1.0f, true), events);
			CHECK(events == std::vector<uint32_t>{ 0, 1, 2 });
			// [0.5, 1] then [0, 0.25) across one wrap: End at the clip's end, then Start again, and the events at 0.25 wait
			// for the next step.
			events.clear();
			CollectAnimationEvents(clip, AdvanceAnimationTime(0.5f, 0.75f, 1.0f, true), events);
			CHECK(events == std::vector<uint32_t>{ 3, 0 });
			// Two wraps from 0.5: [0.5, 1], a whole cycle [0, 1] and [0, 0.5).
			events.clear();
			CollectAnimationEvents(clip, AdvanceAnimationTime(0.5f, 2.0f, 1.0f, true), events);
			CHECK(events == std::vector<uint32_t>{ 3, 0, 1, 2, 3, 0, 1, 2 });
			// A step that ends exactly at the end wraps to 0: End fires, and Start waits for the next step.
			events.clear();
			CollectAnimationEvents(clip, AdvanceAnimationTime(0.5f, 0.5f, 1.0f, true), events);
			CHECK(events == std::vector<uint32_t>{ 3 });
			// A fuzz-shaped clip, the shortest at the fastest speed with an event at each end, fires each event at most once for
			// each of the MaxAnimationWrapsPerStep ends counted, plus the cycle the step ends in.
			AnimationClipData flicker;
			flicker.Duration = MinAnimationClipDuration;
			flicker.Events = { AnimationEvent{ .Name = "Open", .Time = 0.0f }, AnimationEvent{ .Name = "Close", .Time = MinAnimationClipDuration } };
			events.clear();
			CollectAnimationEvents(flicker, AdvanceAnimationTime(0.0f, 100.0f / 60.0f, MinAnimationClipDuration, true), events);
			CHECK_FALSE(events.empty());
			CHECK(events.size() <= (MaxAnimationWrapsPerStep + 1) * flicker.Events.size());
			// A held clip that reaches its end fires the event at its end, once.
			events.clear();
			CollectAnimationEvents(clip, AdvanceAnimationTime(0.75f, 0.5f, 1.0f, false), events);
			CHECK(events == std::vector<uint32_t>{ 3 });
			events.clear();
			CollectAnimationEvents(clip, AdvanceAnimationTime(1.0f, 0.5f, 1.0f, false), events);
			CHECK(events.empty());
			// A step of zero delta fires nothing.
			CollectAnimationEvents(clip, AdvanceAnimationTime(0.25f, 0.0f, 1.0f, true), events);
			CHECK(events.empty());
		}
	}

}
