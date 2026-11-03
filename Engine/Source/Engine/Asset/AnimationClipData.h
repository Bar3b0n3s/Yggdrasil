#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Animation clips (M23, C23.3, C-SKEL, C-MORPH, C-ANIM-BLEND; Docs/Decisions/0034-m23-contract.md decisions 23 and 24): a glTF
// animation as a cooked asset (AssetType::AnimationClip, format version 1), produced by GltfImporter version 3 as the
// sub-asset "animation:<index>[:<name>]" and played by an Animator's layers (Animation/AnimationPose.h). A clip is data only:
//   - Channels key one property each: a joint's Translation, Rotation or Scale, the joint named by Target (a Skeleton's
//     joint name, so a clip plays on any skeleton whose joints carry its names; a channel whose joint the skeleton lacks is
//     ignored), or the morph Weights of the SkinnedMesh whose entity Target names: the '/'-separated entity names of the
//     path from the Animator's entity to it (empty: the Animator's own entity), glTF's node names as the importer makes them.
//   - Keys: Times in seconds, strictly increasing, from 0; Values per key, as glTF lays them out: 3 floats for Translation
//     and Scale, 4 for Rotation (a quaternion x, y, z, w), WeightCount for Weights; with CubicSpline each key holds an
//     in-tangent, the value and an out-tangent, in that order (three times as many floats). Before the first key a channel
//     holds its first value and after the last its last value.
//   - Duration: the clip's length in seconds, at least its last key and its last event (glTF: the largest key time), 0 or
//     at least MinAnimationClipDuration; a looping layer wraps there.
//   - Events: named points of time (the import settings' AnimationEvents, GltfImporter.h), sorted by Time (stable), which
//     the AnimationSystem phase delivers to OnAnimationEvent when a layer's time passes them (Animation/AnimationSampling.h).
//
// Frozen by the M23 contract C23.3; stream A implements the payload and the validation.

namespace Engine {

	// The limits ValidateAnimationClipData enforces, so a clip that external input controls cannot exhaust memory.
	inline constexpr uint32_t MaxAnimationChannels = 4096;
	inline constexpr uint32_t MaxAnimationChannelKeys = 1U << 20;
	inline constexpr uint32_t MaxAnimationClipFloats = 1U << 26; // every channel's Times and Values together (256 MiB)
	inline constexpr uint32_t MaxAnimationEvents = 4096;
	inline constexpr uint32_t MaxAnimationNameBytes = 256;      // a channel's Target and an event's Name, bytes of UTF-8
	inline constexpr float MaxAnimationClipDuration = 86400.0f; // seconds (a day)
	// The shortest clip that is not a single pose (decision 24): a Duration is 0 (every key at 0: a pose that never wraps) or
	// at least this, so the ends a looping layer passes in one fixed step stay bounded (and it counts at most
	// MaxAnimationWrapsPerStep of them, Animation/AnimationSampling.h). A cycle of a millisecond is already shorter than a
	// rendered frame, so no clip that plays visibly is refused; GltfImporter skips a shorter animation (GltfImporter.h).
	inline constexpr float MinAnimationClipDuration = 0.001f; // seconds

	// What a channel keys. Persisted as a byte in the cooked payload (append only).
	enum class AnimationPath : uint8_t
	{
		Translation,
		Rotation,
		Scale,
		Weights
	};

	// How a channel interpolates between keys, as glTF's sampler (glTF 2.0 §3.11, Appendix C). Persisted as a byte (append
	// only).
	enum class AnimationInterpolation : uint8_t
	{
		Step,       // the earlier key's value
		Linear,     // linear; Rotation spherical linear along the shorter arc (Animation/AnimationSampling.h)
		CubicSpline // cubic Hermite with the keys' tangents scaled by the key interval; Rotation normalized after
	};

	// One keyed property (see the file comment).
	struct AnimationChannel
	{
		std::string Target{}; // a joint's name (Translation, Rotation, Scale; 1 to MaxAnimationNameBytes bytes) or an entity path (Weights; may be empty)
		AnimationPath Path = AnimationPath::Translation;
		AnimationInterpolation Interpolation = AnimationInterpolation::Linear;
		uint32_t WeightCount = 0;   // Weights: the morph targets each key holds, 1 to MaxMeshMorphTargets; 0 for the other paths
		std::vector<float> Times{}; // 1 to MaxAnimationChannelKeys keys
		std::vector<float> Values{};

		bool operator==(const AnimationChannel&) const = default;
	};

	// One named point of a clip's time (see the file comment).
	struct AnimationEvent
	{
		std::string Name{}; // 1 to MaxAnimationNameBytes bytes of UTF-8
		float Time = 0.0f;  // seconds, in [0, Duration]

		bool operator==(const AnimationEvent&) const = default;
	};

	// A loaded clip (AssetType::AnimationClip). Plain data; immutable once loaded (AssetRef<AnimationClipData>).
	struct AnimationClipData : Asset
	{
		static constexpr AssetType StaticType = AssetType::AnimationClip;
		// The cooked payload layout below (CookedHeader::FormatVersion; ADR 0028's phase-2 ledger: a new format at version 1).
		static constexpr uint16_t FormatVersion = 1;

		AnimationClipData()
			: Asset(StaticType)
		{
		}

		float Duration = 0.0f; // seconds: 0, or in [MinAnimationClipDuration, MaxAnimationClipDuration]
		std::vector<AnimationChannel> Channels{};
		std::vector<AnimationEvent> Events{};
	};

	// The number of floats one key of `channel` holds: 3, 4 or WeightCount for its path, times 3 for CubicSpline. Pure.
	[[nodiscard]] uint32_t GetAnimationKeyFloatCount(const AnimationChannel& channel);

	// The invariants every producer guarantees and every reader re-checks on untrusted data: at most MaxAnimationChannels
	// channels, each with a Target as its path needs it (a joint channel's 1 to MaxAnimationNameBytes bytes, a Weights
	// channel's at most that many, valid UTF-8), its WeightCount as its path needs it, 1 to MaxAnimationChannelKeys Times,
	// finite, strictly increasing, from 0 to at most Duration, and GetAnimationKeyFloatCount floats per key, finite, a Linear
	// or Step Rotation key of unit length within 1e-3; no two channels with the same Target and Path; at most
	// MaxAnimationClipFloats floats in all; Duration 0 or finite in [MinAnimationClipDuration, MaxAnimationClipDuration] (a
	// positive Duration below the minimum is refused); at most MaxAnimationEvents events, each Name valid UTF-8 of 1 to
	// MaxAnimationNameBytes bytes and each Time in [0, Duration], sorted by Time. Pure. Errors: Validation naming the first
	// violation and its channel or event ("channel 3 'Spine' Rotation: key 7's time 0.5 does not follow key 6's 0.5").
	[[nodiscard]] Status ValidateAnimationClipData(const AnimationClipData& clip);

	// The cooked clip payload (FormatVersion 1), little-endian, a string as a uint32 byte count then its UTF-8 bytes:
	//     f32 Duration; uint32 ChannelCount; uint32 EventCount;
	//     ChannelCount x { string Target; uint8 Path; uint8 Interpolation; uint16 Reserved (0); uint32 WeightCount;
	//                      uint32 KeyCount; KeyCount x f32 Time; KeyCount x GetAnimationKeyFloatCount f32 Value }
	//     EventCount x { string Name; f32 Time; }
	// Channels and events in the clip's order. Asserts ValidateAnimationClipData. Pure; identical clips give identical bytes
	// in every configuration (no padding, floats as their bits).
	[[nodiscard]] Buffer SerializeAnimationClipPayload(const AnimationClipData& clip);

	// Reads a payload written by SerializeAnimationClipPayload, checking every count and length against its limit and
	// against the bytes left before allocating, then ValidateAnimationClipData. Never asserts on data (fuzzed:
	// "AnimationClipData: 10,000 seeded mutations of a payload never crash", the fuzz target AnimationClipData). Errors:
	// Parse for truncation, a count above its limit, a Path or Interpolation byte outside its enumeration, a non-zero
	// Reserved word or trailing bytes; Validation from ValidateAnimationClipData.
	[[nodiscard]] Result<AnimationClipData> DeserializeAnimationClipPayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload, §6.8) of `clip`, produced by a source of version
	// `importerVersion`. Asserts ValidateAnimationClipData (through SerializeAnimationClipPayload).
	[[nodiscard]] Buffer CookAnimationClip(const AnimationClipData& clip, uint32_t importerVersion);

	// The clip of a cooked artifact (both asset managers' loader for AssetType::AnimationClip, RegisterBuiltinLoaders):
	// ReadCookedArtifact with AssetType::AnimationClip and FormatVersion, then the payload. Errors: as ReadCookedArtifact and
	// DeserializeAnimationClipPayload.
	[[nodiscard]] Result<AssetRef<AnimationClipData>> LoadCookedAnimationClip(std::span<const std::byte> cooked);

}
