#include "EnginePCH.h"
#include "Engine/Animation/AnimationSampling.h"

#include "Engine/Core/DetMath.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

// Joint transforms, quaternion interpolation, layer time and events (AnimationSampling.h). IEEE-exact operations, sqrt
// and DetMath only (the file comment of the header).

namespace Engine {

	namespace Utils {

		// `time` brought into [0, duration] the way a layer's time comes into its clip (AdvanceAnimationTime): a looping
		// layer's by subtracting whole durations in double, another's clamped. `cycles` receives the whole durations
		// subtracted (the ends a looping time passed), saturated at `maxCycles` before its conversion. A wrapped time that
		// rounds to `duration` in float is the start of the next cycle.
		static float WrapAnimationTime(double time, double duration, bool loop, uint32_t maxCycles, uint32_t& cycles)
		{
			cycles = 0;
			if (!(time > 0.0))
				return 0.0f;
			if (!loop)
				return static_cast<float>(std::min(time, duration));
			double whole = std::floor(time / duration);
			// A huge quotient rounds: the remainder may fall just outside [0, duration).
			double wrapped = std::max(time - duration * whole, 0.0);
			if (static_cast<float>(wrapped) >= static_cast<float>(duration))
			{
				wrapped = 0.0;
				whole += 1.0;
			}
			const double saturated = std::min(whole, static_cast<double>(maxCycles));
			cycles = saturated > 0.0 ? static_cast<uint32_t>(saturated) : 0;
			return static_cast<float>(wrapped);
		}

		// The events of `clip` whose time e satisfies from <= e < to (or e <= to with `includeEnd`), in clip order, appended
		// to `events`.
		static void AppendEventsBetween(const AnimationClipData& clip, float from, float to, bool includeEnd, std::vector<uint32_t>& events)
		{
			for (size_t index = 0; index < clip.Events.size(); ++index)
			{
				const float time = clip.Events[index].Time;
				if (time >= from && (time < to || (includeEnd && time <= to)))
					events.push_back(static_cast<uint32_t>(index));
			}
		}

	}

	glm::mat4 ComposeJointTransform(const JointTransform& transform)
	{
		glm::mat4 matrix = glm::mat4_cast(transform.Rotation);
		matrix[0] *= transform.Scale.x;
		matrix[1] *= transform.Scale.y;
		matrix[2] *= transform.Scale.z;
		matrix[3] = glm::vec4(transform.Translation, 1.0f);
		return matrix;
	}

	glm::quat SlerpQuaternion(const glm::quat& from, const glm::quat& to, float t)
	{
		const double clamped = std::clamp(static_cast<double>(t), 0.0, 1.0);
		double dot = static_cast<double>(from.x) * to.x + static_cast<double>(from.y) * to.y + static_cast<double>(from.z) * to.z
			+ static_cast<double>(from.w) * to.w;
		// The shorter arc: q and -q are the same rotation.
		const glm::quat target = dot < 0.0 ? -to : to;
		dot = std::abs(dot);
		// The ends exactly, so a key's value is reproduced bit for bit.
		if (clamped <= 0.0)
			return from;
		if (clamped >= 1.0)
			return target;
		double fromScale = 1.0 - clamped;
		double toScale = clamped;
		if (dot < 1.0 - 1e-4)
		{
			const double angle = DetMath::ACos(std::min(dot, 1.0));
			const double sine = DetMath::Sin(angle);
			fromScale = DetMath::Sin((1.0 - clamped) * angle) / sine;
			toScale = DetMath::Sin(clamped * angle) / sine;
		}
		const double x = fromScale * from.x + toScale * target.x;
		const double y = fromScale * from.y + toScale * target.y;
		const double z = fromScale * from.z + toScale * target.z;
		const double w = fromScale * from.w + toScale * target.w;
		const double length = std::sqrt(x * x + y * y + z * z + w * w);
		if (!(length > 0.0))
			return from;
		return glm::quat(static_cast<float>(w / length), static_cast<float>(x / length), static_cast<float>(y / length), static_cast<float>(z / length));
	}

	AnimationTimeStep AdvanceAnimationTime(float time, float delta, float duration, bool loop)
	{
		AnimationTimeStep step;
		if (!(duration > 0.0f))
			return step;
		const double length = static_cast<double>(duration);
		uint32_t ignored = 0;
		step.PreviousTime = Utils::WrapAnimationTime(static_cast<double>(time), length, loop, MaxAnimationWrapsPerStep, ignored);
		const double advanced = static_cast<double>(step.PreviousTime) + (delta > 0.0f ? static_cast<double>(delta) : 0.0);
		if (loop)
		{
			step.Time = Utils::WrapAnimationTime(advanced, length, true, MaxAnimationWrapsPerStep, step.Wraps);
			return step;
		}
		step.Time = static_cast<float>(std::min(advanced, length));
		step.Ended = step.PreviousTime < duration && step.Time >= duration;
		return step;
	}

	void CollectAnimationEvents(const AnimationClipData& clip, const AnimationTimeStep& step, std::vector<uint32_t>& events)
	{
		if (clip.Events.empty())
			return;
		if (step.Wraps > 0)
		{
			Utils::AppendEventsBetween(clip, step.PreviousTime, clip.Duration, true, events);
			for (uint32_t cycle = 1; cycle < step.Wraps; ++cycle)
				Utils::AppendEventsBetween(clip, 0.0f, clip.Duration, true, events);
			Utils::AppendEventsBetween(clip, 0.0f, step.Time, false, events);
			return;
		}
		if (step.Ended)
		{
			Utils::AppendEventsBetween(clip, step.PreviousTime, clip.Duration, true, events);
			return;
		}
		Utils::AppendEventsBetween(clip, step.PreviousTime, step.Time, false, events);
	}

}
