#include "TestsPCH.h"

#include "Engine/Asset/AnimationClipData.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Random.h"
#include "Support/Fuzz.h"

#include <cstdint>
#include <limits>
#include <utility>

// Animation clips (M23, C23.3, C-SKEL, C-MORPH, C-ANIM-BLEND; Docs/Decisions/0034-m23-contract.md decision 24): the cooked
// format version 1, its validation and its fuzz target.

namespace Engine {

	namespace {

		// A one-second clip: the elbow's rotation keyed linearly (identity, then 90 degrees about +Z), the shoulder's translation
		// stepped, the face's two morph weights as a cubic spline, and two events.
		AnimationClipData MakeWave()
		{
			AnimationClipData clip;
			clip.Duration = 1.0f;
			clip.Channels.push_back(AnimationChannel{ .Target = "Elbow", .Path = AnimationPath::Rotation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 1.0f }, .Values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.70710678f, 0.70710678f } });
			clip.Channels.push_back(AnimationChannel{ .Target = "Shoulder", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Step, .Times = { 0.0f, 0.5f }, .Values = { 0.0f, 0.0f, 0.0f, 0.0f, 0.25f, 0.0f } });
			clip.Channels.push_back(AnimationChannel{ .Target = "Body/Face", .Path = AnimationPath::Weights, .Interpolation = AnimationInterpolation::CubicSpline, .WeightCount = 2, .Times = { 0.0f, 1.0f }, .Values = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f } });
			clip.Events = { AnimationEvent{ .Name = "Raise", .Time = 0.25f }, AnimationEvent{ .Name = "Done", .Time = 1.0f } };
			return clip;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("AnimationClipData: a key holds three, four or WeightCount floats by path, three times as many for a cubic spline")
		{
			CHECK(GetAnimationKeyFloatCount(AnimationChannel{ .Path = AnimationPath::Translation }) == 3);
			CHECK(GetAnimationKeyFloatCount(AnimationChannel{ .Path = AnimationPath::Scale, .Interpolation = AnimationInterpolation::Step }) == 3);
			CHECK(GetAnimationKeyFloatCount(AnimationChannel{ .Path = AnimationPath::Rotation }) == 4);
			CHECK(GetAnimationKeyFloatCount(AnimationChannel{ .Path = AnimationPath::Weights, .WeightCount = 5 }) == 5);
			CHECK(GetAnimationKeyFloatCount(AnimationChannel{ .Path = AnimationPath::Rotation, .Interpolation = AnimationInterpolation::CubicSpline }) == 12);
			CHECK(GetAnimationKeyFloatCount(AnimationChannel{ .Path = AnimationPath::Weights, .Interpolation = AnimationInterpolation::CubicSpline, .WeightCount = 2 }) == 6);
		}

		TEST_CASE("AnimationClipData: the artifact names its type and format version, and its loader is registered")
		{
			CHECK(AnimationClipData::StaticType == AssetType::AnimationClip);
			CHECK(AnimationClipData::FormatVersion == 1);
			CHECK(GetCookedFormatVersion(AssetType::AnimationClip) == AnimationClipData::FormatVersion);
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			REQUIRE(loaders.Find(AssetType::AnimationClip) != nullptr);
			const std::byte payload[1] = {};
			const Result<AssetRef<AnimationClipData>> newer =
				LoadCookedAnimationClip(WriteCookedArtifact(AssetType::AnimationClip, AnimationClipData::FormatVersion + 1, 3, payload));
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("AnimationClipData: a payload round-trips its channels and events")
		{
			const AnimationClipData wave = MakeWave();
			REQUIRE(ValidateAnimationClipData(wave).has_value());
			const Buffer payload = SerializeAnimationClipPayload(wave);
			const Result<AnimationClipData> read = DeserializeAnimationClipPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Duration == wave.Duration);
			CHECK(read->Channels == wave.Channels);
			CHECK(read->Events == wave.Events);
			CHECK(SerializeAnimationClipPayload(*read) == payload);

			const Result<AssetRef<AnimationClipData>> loaded = LoadCookedAnimationClip(CookAnimationClip(wave, 3));
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK((*loaded)->Channels == wave.Channels);
		}

		TEST_CASE("AnimationClipData: channels and events that break the format's rules are refused")
		{
			const auto refused = [](const AnimationClipData& clip)
			{
				const Status status = ValidateAnimationClipData(clip);
				return !status.has_value() && status.error().GetCode() == ErrorCode::Validation;
			};
			AnimationClipData unsorted = MakeWave();
			unsorted.Channels[0].Times = { 0.5f, 0.5f };
			CHECK(refused(unsorted));
			AnimationClipData pastEnd = MakeWave();
			pastEnd.Channels[1].Times = { 0.0f, 1.5f };
			CHECK(refused(pastEnd));
			AnimationClipData shortValues = MakeWave();
			shortValues.Channels[0].Values.pop_back();
			CHECK(refused(shortValues));
			AnimationClipData denormal = MakeWave();
			denormal.Channels[0].Values[3] = 2.0f; // a linear rotation key of length 2
			CHECK(refused(denormal));
			AnimationClipData unnamedJoint = MakeWave();
			unnamedJoint.Channels[0].Target.clear();
			CHECK(refused(unnamedJoint));
			AnimationClipData twice = MakeWave();
			twice.Channels.push_back(twice.Channels[0]);
			CHECK(refused(twice));
			AnimationClipData noWeights = MakeWave();
			noWeights.Channels[2].WeightCount = 0;
			CHECK(refused(noWeights));
			AnimationClipData lateEvent = MakeWave();
			lateEvent.Events[1].Time = 2.0f;
			CHECK(refused(lateEvent));
			AnimationClipData unsortedEvents = MakeWave();
			std::swap(unsortedEvents.Events[0], unsortedEvents.Events[1]);
			CHECK(refused(unsortedEvents));
			AnimationClipData nan = MakeWave();
			nan.Channels[1].Values[4] = std::numeric_limits<float>::quiet_NaN();
			CHECK(refused(nan));
			// A clip shorter than MinAnimationClipDuration but not a single pose (a fuzz-shaped clip whose keys are a microsecond
			// apart) is refused; a clip of duration 0 whose keys are all at 0 is a pose and valid.
			AnimationClipData flicker;
			flicker.Duration = 1.0e-6f;
			flicker.Channels.push_back(AnimationChannel{ .Target = "Shoulder", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Step, .Times = { 0.0f, 1.0e-6f }, .Values = { 0.0f, 0.0f, 0.0f, 0.0f, 0.25f, 0.0f } });
			CHECK(refused(flicker));
			AnimationClipData pose;
			pose.Channels.push_back(AnimationChannel{ .Target = "Shoulder", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Step, .Times = { 0.0f }, .Values = { 0.0f, 0.25f, 0.0f } });
			CHECK(ValidateAnimationClipData(pose).has_value());
			AnimationClipData shortest = flicker;
			shortest.Duration = MinAnimationClipDuration;
			shortest.Channels[0].Times[1] = MinAnimationClipDuration;
			CHECK(ValidateAnimationClipData(shortest).has_value());
			// A Weights channel may target the Animator's own entity (an empty path).
			AnimationClipData own = MakeWave();
			own.Channels[2].Target.clear();
			CHECK(ValidateAnimationClipData(own).has_value());

			Buffer reserved = SerializeAnimationClipPayload(MakeWave());
			REQUIRE(reserved.size() > 24);
			// The first channel's Reserved word: after Duration, the counts, the target string "Elbow" and the two path bytes.
			reserved[4 + 4 + 4 + 4 + 5 + 2] = std::byte{ 1 };
			const Result<AnimationClipData> read = DeserializeAnimationClipPayload(reserved);
			REQUIRE_FALSE(read.has_value());
			CHECK(read.error().GetCode() == ErrorCode::Parse);
		}

		TEST_CASE("AnimationClipData: 10,000 seeded mutations of a payload never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			const Buffer payload = SerializeAnimationClipPayload(MakeWave());
			REQUIRE(DeserializeAnimationClipPayload(payload).has_value());
			const Test::FuzzRun run = Test::RunFuzzCase(0xA41C, 10000, [&payload](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = payload;
				Test::MutateBytes(mutated, random);
				// A payload the reader accepts is a valid clip.
				const Result<AnimationClipData> read = DeserializeAnimationClipPayload(mutated);
				if (!read.has_value())
					return false;
				CHECK(ValidateAnimationClipData(*read).has_value());
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}
	}

}
