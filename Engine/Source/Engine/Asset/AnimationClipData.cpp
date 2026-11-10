#include "EnginePCH.h"
#include "Engine/Asset/AnimationClipData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Utf8.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

// The animation clip's cooked format (AnimationClipData.h; Docs/Decisions/0034-m23-contract.md decision 24): the key layout
// and the artifact wrappers are the contract's, the payload and the validation stream A's.

namespace Engine {

	namespace Utils {

		// How far a Step or Linear rotation key may be from unit length (ValidateAnimationClipData).
		static constexpr double ClipUnitRotationTolerance = 1e-3;

		// The fewest bytes a channel record and an event record take in a payload (an empty string's length word, the path,
		// interpolation and reserved bytes, the weight and key counts; an event's name length and time), for the size checks
		// before anything is allocated.
		static constexpr uint64_t MinChannelPayloadBytes = 4 + 1 + 1 + 2 + 4 + 4;
		static constexpr uint64_t MinEventPayloadBytes = 4 + 4;

		[[nodiscard]] static std::string_view GetPathName(AnimationPath path)
		{
			switch (path)
			{
				case AnimationPath::Translation: return "Translation";
				case AnimationPath::Rotation:    return "Rotation";
				case AnimationPath::Scale:       return "Scale";
				case AnimationPath::Weights:     return "Weights";
			}
			return "an unknown path";
		}

		// Checks one channel (`index` in the clip) against the clip's duration; `floats` accumulates its Times and Values.
		[[nodiscard]] static Status ValidateChannel(const AnimationChannel& channel, size_t index, float duration, uint64_t& floats)
		{
			const std::string_view path = GetPathName(channel.Path);
			const auto fail = [&channel, index, path](std::string_view problem)
			{
				return MakeError(ErrorCode::Validation, "channel {} '{}' {}: {}", index, channel.Target, path, problem);
			};
			if (std::to_underlying(channel.Path) > std::to_underlying(AnimationPath::Weights))
				return MakeError(ErrorCode::Validation, "channel {} has a path outside the enumeration", index);
			if (std::to_underlying(channel.Interpolation) > std::to_underlying(AnimationInterpolation::CubicSpline))
				return MakeError(ErrorCode::Validation, "channel {} has an interpolation outside the enumeration", index);
			if (channel.Target.size() > MaxAnimationNameBytes || !IsValidUtf8(channel.Target))
			{
				return MakeError(ErrorCode::Validation, "channel {} has a target that is not at most {} bytes of valid UTF-8", index,
					MaxAnimationNameBytes);
			}
			if (channel.Path == AnimationPath::Weights)
			{
				if (channel.WeightCount == 0 || channel.WeightCount > MaxMeshMorphTargets)
					return fail(std::format("keys {} weights, not 1 to {}", channel.WeightCount, MaxMeshMorphTargets));
			}
			else
			{
				if (channel.Target.empty())
					return MakeError(ErrorCode::Validation, "channel {} {} names no joint", index, path);
				if (channel.WeightCount != 0)
					return fail(std::format("has a weight count of {}; only a Weights channel has one", channel.WeightCount));
			}

			const size_t keyCount = channel.Times.size();
			if (keyCount == 0 || keyCount > MaxAnimationChannelKeys)
				return fail(std::format("has {} keys, not 1 to {}", keyCount, MaxAnimationChannelKeys));
			const uint64_t keyFloats = GetAnimationKeyFloatCount(channel);
			if (channel.Values.size() != keyCount * keyFloats)
				return fail(std::format("has {} values for its {} keys of {} floats", channel.Values.size(), keyCount, keyFloats));
			floats += keyCount + channel.Values.size();

			for (size_t key = 0; key < keyCount; ++key)
			{
				const float time = channel.Times[key];
				if (!std::isfinite(time) || time < 0.0f || time > duration)
					return fail(std::format("key {}'s time {} is not in [0, {}]", key, time, duration));
				if (key > 0 && !(time > channel.Times[key - 1]))
					return fail(std::format("key {}'s time {} does not follow key {}'s {}", key, time, key - 1, channel.Times[key - 1]));
			}
			for (size_t value = 0; value < channel.Values.size(); ++value)
			{
				if (!std::isfinite(channel.Values[value]))
					return fail(std::format("key {} has a value that is not a finite number", value / keyFloats));
			}
			if (channel.Path == AnimationPath::Rotation && channel.Interpolation != AnimationInterpolation::CubicSpline)
			{
				for (size_t key = 0; key < keyCount; ++key)
				{
					const float* rotation = channel.Values.data() + key * 4;
					const double x = rotation[0];
					const double y = rotation[1];
					const double z = rotation[2];
					const double w = rotation[3];
					if (std::fabs(std::sqrt(x * x + y * y + z * z + w * w) - 1.0) > ClipUnitRotationTolerance)
						return fail(std::format("key {}'s rotation is not unit length", key));
				}
			}
			return {};
		}

		[[nodiscard]] static Status CheckClipPayloadCount(const BinaryReader& reader, uint64_t count, uint64_t limit, uint64_t minimumBytes,
			std::string_view what)
		{
			if (count > limit)
			{
				return MakeError(ErrorCode::Parse, "animation clip payload: {} {} at offset {}, more than the {} a clip may have",
					count, what, reader.GetPosition(), limit);
			}
			if (count * minimumBytes > reader.GetRemaining())
			{
				return MakeError(ErrorCode::Parse, "animation clip payload: {} {} need at least {} bytes, but {} follow at offset {}", count, what, count * minimumBytes,
					reader.GetRemaining(), reader.GetPosition());
			}
			return {};
		}

		// Reads one channel; `floats` accumulates the floats read so far, against MaxAnimationClipFloats.
		[[nodiscard]] static Result<AnimationChannel> ReadChannel(BinaryReader& reader, uint32_t index, uint64_t& floats)
		{
			AnimationChannel channel;
			ENGINE_TRY_ASSIGN(channel.Target, WithContext(reader.ReadString(), std::format("in the target of channel {}", index)));
			const size_t enumOffset = reader.GetPosition();
			ENGINE_TRY_ASSIGN(const uint8_t path, reader.ReadU8());
			ENGINE_TRY_ASSIGN(const uint8_t interpolation, reader.ReadU8());
			ENGINE_TRY_ASSIGN(const uint16_t reserved, reader.ReadU16());
			if (path > std::to_underlying(AnimationPath::Weights))
			{
				return MakeError(ErrorCode::Parse,
					"animation clip payload: channel {}'s path byte at offset {} is {}, outside the enumeration", index, enumOffset, path);
			}
			if (interpolation > std::to_underlying(AnimationInterpolation::CubicSpline))
			{
				return MakeError(ErrorCode::Parse, "animation clip payload: channel {}'s interpolation byte at offset {} is {}, outside the enumeration", index,
					enumOffset + 1, interpolation);
			}
			if (reserved != 0)
			{
				return MakeError(ErrorCode::Parse, "animation clip payload: channel {}'s reserved word at offset {} is {:#x}, not 0",
					index, enumOffset + 2, reserved);
			}
			channel.Path = static_cast<AnimationPath>(path);
			channel.Interpolation = static_cast<AnimationInterpolation>(interpolation);
			ENGINE_TRY_ASSIGN(channel.WeightCount, reader.ReadU32());
			if (channel.WeightCount > MaxMeshMorphTargets)
			{
				return MakeError(ErrorCode::Parse, "animation clip payload: channel {} keys {} weights, more than the {} morph targets a mesh may have", index,
					channel.WeightCount, MaxMeshMorphTargets);
			}
			ENGINE_TRY_ASSIGN(const uint32_t keyCount, reader.ReadU32());
			const uint64_t keyFloats = GetAnimationKeyFloatCount(channel);
			const uint64_t channelFloats = static_cast<uint64_t>(keyCount) * (1 + keyFloats);
			if (keyCount > MaxAnimationChannelKeys || floats + channelFloats > MaxAnimationClipFloats)
			{
				return MakeError(ErrorCode::Parse,
					"animation clip payload: channel {} declares {} keys, more than a channel or a clip may hold", index, keyCount);
			}
			if (channelFloats * sizeof(float) > reader.GetRemaining())
			{
				return MakeError(ErrorCode::Parse, "animation clip payload: channel {}'s {} keys need {} bytes, but {} follow at offset {}", index, keyCount,
					channelFloats * sizeof(float), reader.GetRemaining(), reader.GetPosition());
			}
			floats += channelFloats;
			ENGINE_TRY_ASSIGN(channel.Times, reader.ReadArray<float>(keyCount));
			ENGINE_TRY_ASSIGN(channel.Values, reader.ReadArray<float>(static_cast<size_t>(keyCount * keyFloats)));
			return channel;
		}

	}

	uint32_t GetAnimationKeyFloatCount(const AnimationChannel& channel)
	{
		uint32_t count = 0;
		switch (channel.Path)
		{
			case AnimationPath::Translation:
			case AnimationPath::Scale:       count = 3; break;
			case AnimationPath::Rotation:    count = 4; break;
			case AnimationPath::Weights:     count = channel.WeightCount; break;
		}
		return channel.Interpolation == AnimationInterpolation::CubicSpline ? 3 * count : count;
	}

	Status ValidateAnimationClipData(const AnimationClipData& clip)
	{
		const bool pose = clip.Duration == 0.0f;
		if (!std::isfinite(clip.Duration) || (!pose && (clip.Duration < MinAnimationClipDuration || clip.Duration > MaxAnimationClipDuration)))
		{
			return MakeError(ErrorCode::Validation, "the clip's duration {} is neither 0 nor in [{}, {}] seconds", clip.Duration, MinAnimationClipDuration,
				MaxAnimationClipDuration);
		}
		if (clip.Channels.size() > MaxAnimationChannels)
		{
			return MakeError(ErrorCode::Validation, "the clip has {} channels, more than the {} a clip may have", clip.Channels.size(),
				MaxAnimationChannels);
		}

		uint64_t floats = 0;
		std::vector<std::pair<std::pair<std::string_view, AnimationPath>, size_t>> targets;
		targets.reserve(clip.Channels.size());
		for (size_t index = 0; index < clip.Channels.size(); ++index)
		{
			const AnimationChannel& channel = clip.Channels[index];
			ENGINE_TRY(Utils::ValidateChannel(channel, index, clip.Duration, floats));
			if (floats > MaxAnimationClipFloats)
				return MakeError(ErrorCode::Validation, "the clip's keys hold more than the {} floats a clip may have", MaxAnimationClipFloats);
			targets.emplace_back(std::pair(std::string_view(channel.Target), channel.Path), index);
		}
		std::ranges::sort(targets);
		for (size_t position = 1; position < targets.size(); ++position)
		{
			if (targets[position].first == targets[position - 1].first)
			{
				const AnimationChannel& channel = clip.Channels[targets[position].second];
				return MakeError(ErrorCode::Validation, "channel {} '{}' {} keys what channel {} keys; a clip keys each target and path once", targets[position].second,
					channel.Target, Utils::GetPathName(channel.Path), targets[position - 1].second);
			}
		}

		if (clip.Events.size() > MaxAnimationEvents)
		{
			return MakeError(ErrorCode::Validation, "the clip has {} events, more than the {} a clip may have", clip.Events.size(),
				MaxAnimationEvents);
		}
		for (size_t index = 0; index < clip.Events.size(); ++index)
		{
			const AnimationEvent& event = clip.Events[index];
			if (event.Name.empty() || event.Name.size() > MaxAnimationNameBytes || !IsValidUtf8(event.Name))
				return MakeError(ErrorCode::Validation, "event {} has a name that is not 1 to {} bytes of valid UTF-8", index, MaxAnimationNameBytes);
			if (!std::isfinite(event.Time) || event.Time < 0.0f || event.Time > clip.Duration)
			{
				return MakeError(ErrorCode::Validation, "event {} '{}' is at {} s, outside the clip's [0, {}]", index, event.Name,
					event.Time, clip.Duration);
			}
			if (index > 0 && event.Time < clip.Events[index - 1].Time)
			{
				return MakeError(ErrorCode::Validation, "event {} '{}' at {} s precedes event {} at {} s; events are sorted by time", index, event.Name, event.Time,
					index - 1, clip.Events[index - 1].Time);
			}
		}
		return {};
	}

	Buffer SerializeAnimationClipPayload(const AnimationClipData& clip)
	{
		ENGINE_CORE_ASSERT(ValidateAnimationClipData(clip).has_value(),
			"SerializeAnimationClipPayload needs a clip that passes ValidateAnimationClipData");

		BinaryWriter writer;
		writer.WriteF32(clip.Duration);
		writer.WriteU32(static_cast<uint32_t>(clip.Channels.size()));
		writer.WriteU32(static_cast<uint32_t>(clip.Events.size()));
		for (const AnimationChannel& channel : clip.Channels)
		{
			writer.WriteString(channel.Target);
			writer.WriteU8(std::to_underlying(channel.Path));
			writer.WriteU8(std::to_underlying(channel.Interpolation));
			writer.WriteU16(0); // Reserved
			writer.WriteU32(channel.WeightCount);
			writer.WriteU32(static_cast<uint32_t>(channel.Times.size()));
			writer.WriteArray(std::span<const float>(channel.Times));
			writer.WriteArray(std::span<const float>(channel.Values));
		}
		for (const AnimationEvent& event : clip.Events)
		{
			writer.WriteString(event.Name);
			writer.WriteF32(event.Time);
		}
		return writer.TakeBuffer();
	}

	Result<AnimationClipData> DeserializeAnimationClipPayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		AnimationClipData clip;
		ENGINE_TRY_ASSIGN(clip.Duration, reader.ReadF32());
		ENGINE_TRY_ASSIGN(const uint32_t channelCount, reader.ReadU32());
		ENGINE_TRY_ASSIGN(const uint32_t eventCount, reader.ReadU32());
		ENGINE_TRY(Utils::CheckClipPayloadCount(reader, channelCount, MaxAnimationChannels, Utils::MinChannelPayloadBytes, "channels"));
		ENGINE_TRY(Utils::CheckClipPayloadCount(reader, eventCount, MaxAnimationEvents, Utils::MinEventPayloadBytes, "events"));
		if (channelCount * Utils::MinChannelPayloadBytes + eventCount * Utils::MinEventPayloadBytes > reader.GetRemaining())
		{
			return MakeError(ErrorCode::Parse, "animation clip payload: {} channels and {} events need more than the {} bytes that follow at offset {}", channelCount,
				eventCount, reader.GetRemaining(), reader.GetPosition());
		}

		uint64_t floats = 0;
		clip.Channels.reserve(channelCount);
		for (uint32_t index = 0; index < channelCount; ++index)
		{
			ENGINE_TRY_ASSIGN(AnimationChannel channel, Utils::ReadChannel(reader, index, floats));
			clip.Channels.push_back(std::move(channel));
		}
		clip.Events.reserve(eventCount);
		for (uint32_t index = 0; index < eventCount; ++index)
		{
			AnimationEvent event;
			ENGINE_TRY_ASSIGN(event.Name, WithContext(reader.ReadString(), std::format("in the name of event {}", index)));
			ENGINE_TRY_ASSIGN(event.Time, reader.ReadF32());
			clip.Events.push_back(std::move(event));
		}
		if (!reader.IsAtEnd())
		{
			return MakeError(ErrorCode::Parse, "animation clip payload has {} trailing bytes after offset {}", reader.GetRemaining(),
				reader.GetPosition());
		}
		ENGINE_TRY(WithContext(ValidateAnimationClipData(clip), "in a cooked animation clip payload"));
		return clip;
	}

	Buffer CookAnimationClip(const AnimationClipData& clip, uint32_t importerVersion)
	{
		const Buffer payload = SerializeAnimationClipPayload(clip);
		return WriteCookedArtifact(AssetType::AnimationClip, AnimationClipData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<AnimationClipData>> LoadCookedAnimationClip(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::AnimationClip, AnimationClipData::FormatVersion));
		ENGINE_TRY_ASSIGN(AnimationClipData clip, DeserializeAnimationClipPayload(artifact.Payload));
		return AssetRef<AnimationClipData>(CreateRef<AnimationClipData>(std::move(clip)));
	}

}
