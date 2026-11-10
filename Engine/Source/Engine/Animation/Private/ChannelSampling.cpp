#include "EnginePCH.h"
#include "Engine/Animation/Private/ChannelSampling.h"

#include "Engine/Animation/AnimationSampling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Channel sampling (ChannelSampling.h).

namespace Engine {

	namespace Utils {

		// The keys around a time: the earlier key, the later one and how far between them the time lies, in [0, 1). Before
		// the first key and from the last one on, both are that key.
		struct AnimationKeySpan
		{
			size_t First = 0;
			size_t Second = 0;
			float Factor = 0.0f;
		};

		// The span of `time` among `times` (at least one key, increasing). A time that is not a number takes the first key and
		// an infinite one an end, so no time indexes past the keys.
		static AnimationKeySpan FindKeySpan(const std::vector<float>& times, float time)
		{
			if (!(time > times.front()))
				return {};
			const size_t last = times.size() - 1;
			if (time >= times[last])
				return { .First = last, .Second = last };
			const size_t second = static_cast<size_t>(std::upper_bound(times.begin(), times.end(), time) - times.begin());
			const size_t first = second - 1;
			const float span = times[second] - times[first];
			return { .First = first, .Second = second, .Factor = span > 0.0f ? (time - times[first]) / span : 0.0f };
		}

		// The `count` value floats of key `key` of `channel`: for CubicSpline its value, between the in-tangent and the
		// out-tangent.
		static const float* KeyValue(const AnimationChannel& channel, size_t key, uint32_t count)
		{
			const bool cubic = channel.Interpolation == AnimationInterpolation::CubicSpline;
			return channel.Values.data() + key * (cubic ? 3 * count : count) + (cubic ? count : 0);
		}

		// Samples `count` components of `channel` at `time` into `out`, component-wise.
		static void SampleComponents(const AnimationChannel& channel, uint32_t count, float time, float* out)
		{
			const AnimationKeySpan span = FindKeySpan(channel.Times, time);
			const float* first = KeyValue(channel, span.First, count);
			if (span.First == span.Second || channel.Interpolation == AnimationInterpolation::Step)
			{
				std::copy(first, first + count, out);
				return;
			}
			const float* second = KeyValue(channel, span.Second, count);
			const float u = span.Factor;
			if (channel.Interpolation == AnimationInterpolation::Linear)
			{
				for (uint32_t component = 0; component < count; ++component)
					out[component] = first[component] + (second[component] - first[component]) * u;
				return;
			}
			// glTF's cubic Hermite spline: the earlier key's value and out-tangent, the later key's value and in-tangent, the
			// tangents scaled by the key interval.
			const float interval = channel.Times[span.Second] - channel.Times[span.First];
			const float* outTangent = first + count;
			const float* inTangent = second - count;
			const float u2 = u * u;
			const float u3 = u2 * u;
			const float startValue = 2.0f * u3 - 3.0f * u2 + 1.0f;
			const float startTangent = (u3 - 2.0f * u2 + u) * interval;
			const float endValue = -2.0f * u3 + 3.0f * u2;
			const float endTangent = (u3 - u2) * interval;
			for (uint32_t component = 0; component < count; ++component)
			{
				out[component] = startValue * first[component] + startTangent * outTangent[component] + endValue * second[component]
					+ endTangent * inTangent[component];
			}
		}

		bool IsReadableAnimationChannel(const AnimationChannel& channel)
		{
			const uint32_t keyFloats = GetAnimationKeyFloatCount(channel);
			return !channel.Times.empty() && keyFloats > 0 && channel.Values.size() >= channel.Times.size() * keyFloats;
		}

		glm::vec3 SampleAnimationVector(const AnimationChannel& channel, float time)
		{
			std::array<float, 3> components{};
			SampleComponents(channel, 3, time, components.data());
			return glm::vec3(components[0], components[1], components[2]);
		}

		glm::quat SampleAnimationRotation(const AnimationChannel& channel, float time)
		{
			const AnimationKeySpan span = FindKeySpan(channel.Times, time);
			const auto read = [&channel](size_t key)
			{
				const float* value = KeyValue(channel, key, 4);
				return glm::quat(value[3], value[0], value[1], value[2]);
			};
			if (span.First == span.Second || channel.Interpolation == AnimationInterpolation::Step)
				return read(span.First);
			if (channel.Interpolation == AnimationInterpolation::Linear)
				return SlerpQuaternion(read(span.First), read(span.Second), span.Factor);
			std::array<float, 4> components{};
			SampleComponents(channel, 4, time, components.data());
			return NormalizeAnimationRotation(glm::quat(components[3], components[0], components[1], components[2]));
		}

		std::vector<float> SampleAnimationWeights(const AnimationChannel& channel, float time)
		{
			std::vector<float> weights(channel.WeightCount);
			SampleComponents(channel, channel.WeightCount, time, weights.data());
			return weights;
		}

		glm::quat NormalizeAnimationRotation(const glm::quat& rotation)
		{
			const float length = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w);
			if (!(length > 0.0f))
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			return glm::quat(rotation.w / length, rotation.x / length, rotation.y / length, rotation.z / length);
		}

		glm::quat BlendAnimationRotation(const glm::quat& from, const glm::quat& to, float weight)
		{
			const float dot = from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
			const glm::quat target = dot < 0.0f ? -to : to;
			const float keep = 1.0f - weight;
			return NormalizeAnimationRotation(glm::quat(from.w * keep + target.w * weight, from.x * keep + target.x * weight,
				from.y * keep + target.y * weight, from.z * keep + target.z * weight));
		}

	}

}
