#include "TestsPCH.h"

#include "Engine/Asset/AudioClipData.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Random.h"
#include "Support/AudioTestData.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

// Audio clip CPU data and its cooked payload (Architecture §6.8, §10.1). The silent clip is implemented by the M12 contract;
// the other tests are skipped skeletons (Docs/Decisions/0015-m12-decisions.md): stream B implements the payload codec and
// the loader and removes the skips.

namespace Engine {

	namespace {

		// The error code of a failed result; nullopt for a success.
		template<typename T>
		std::optional<ErrorCode> GetErrorCode(const Result<T>& result)
		{
			if (result.has_value())
				return std::nullopt;
			return result.error().GetCode();
		}

		// A synthesized-style clip: 0.1 s of a 48 kHz mono tone.
		AudioClipData MakePcmClip()
		{
			const Test::TestToneSpecification tone{ .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 4800, .Frequency = 440.0, .Amplitude = 0.5 };
			AudioClipData clip;
			clip.Encoding = AudioClipEncoding::Pcm16;
			clip.SampleRate = tone.SampleRate;
			clip.ChannelCount = tone.ChannelCount;
			clip.FrameCount = tone.FrameCount;
			clip.Bytes = Test::MakeTonePcm16Bytes(tone);
			return clip;
		}

		// An imported-style clip: the original WAV's bytes, streamed.
		AudioClipData MakeEncodedClip()
		{
			const Test::TestToneSpecification tone{ .SampleRate = 44100, .ChannelCount = 2, .FrameCount = 4410, .Frequency = 440.0, .Amplitude = 0.5 };
			AudioClipData clip;
			clip.Encoding = AudioClipEncoding::Wav;
			clip.SampleRate = tone.SampleRate;
			clip.ChannelCount = tone.ChannelCount;
			clip.FrameCount = tone.FrameCount;
			clip.Stream = true;
			clip.Bytes = Test::MakeToneWav(tone);
			return clip;
		}

		void CheckEqual(const AudioClipData& actual, const AudioClipData& expected)
		{
			CHECK(actual.Encoding == expected.Encoding);
			CHECK(actual.SampleRate == expected.SampleRate);
			CHECK(actual.ChannelCount == expected.ChannelCount);
			CHECK(actual.FrameCount == expected.FrameCount);
			CHECK(actual.Stream == expected.Stream);
			CHECK(actual.Bytes == expected.Bytes);
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("AudioClipData: the silent clip is 0.1 s of 48 kHz mono silence")
		{
			const AudioClipData silence = CreateSilentAudioClip();
			CHECK(silence.GetAssetType() == AssetType::AudioClip);
			CHECK(silence.Encoding == AudioClipEncoding::Pcm16);
			CHECK(silence.SampleRate == 48000);
			CHECK(silence.ChannelCount == 1);
			CHECK(silence.FrameCount == 4800);
			CHECK_FALSE(silence.Stream);
			CHECK(silence.Bytes.size() == 4800 * sizeof(int16_t));
			CHECK(std::ranges::all_of(silence.Bytes, [](std::byte value)
			{
				return value == std::byte{ 0 };
			}));
			CHECK(GetAudioClipDuration(silence) == doctest::Approx(0.1));
			AudioClipData noRate;
			noRate.SampleRate = 0;
			CHECK(GetAudioClipDuration(noRate) == 0.0);
		}

		TEST_CASE("AudioClipData: the encodings name themselves")
		{
			CHECK(AudioClipEncodingToString(AudioClipEncoding::Pcm16) == "Pcm16");
			CHECK(AudioClipEncodingToString(AudioClipEncoding::Wav) == "Wav");
			CHECK(AudioClipEncodingToString(AudioClipEncoding::Flac) == "Flac");
			CHECK(AudioClipEncodingToString(AudioClipEncoding::Mp3) == "Mp3");
			CHECK(AudioClipEncodingToString(AudioClipEncoding::Vorbis) == "Vorbis");
		}

		TEST_CASE("AudioClipData: the payload round-trips for PCM and encoded clips" * doctest::skip(true))
		{
			for (const AudioClipData& clip : { MakePcmClip(), MakeEncodedClip(), CreateSilentAudioClip() })
			{
				REQUIRE(ValidateAudioClipData(clip).has_value());
				const Buffer payload = SerializeAudioClipPayload(clip);
				CHECK(SerializeAudioClipPayload(clip) == payload);
				const Result<AudioClipData> read = DeserializeAudioClipPayload(payload);
				REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
				CheckEqual(*read, clip);
			}
		}

		TEST_CASE("AudioClipData: the payload layout is the documented one" * doctest::skip(true))
		{
			const AudioClipData clip = MakeEncodedClip();
			const Buffer payload = SerializeAudioClipPayload(clip);
			// uint8 Encoding, uint8 Stream, uint16 Reserved, uint32 SampleRate, uint32 ChannelCount, uint64 FrameCount,
			// uint64 ByteCount, then the bytes.
			REQUIRE(payload.size() == 28 + clip.Bytes.size());
			CHECK(payload[0] == std::byte{ static_cast<uint8_t>(AudioClipEncoding::Wav) });
			CHECK(payload[1] == std::byte{ 1 });
			CHECK(payload[2] == std::byte{ 0 });
			CHECK(payload[3] == std::byte{ 0 });
			CHECK(std::equal(clip.Bytes.begin(), clip.Bytes.end(), payload.begin() + 28));
		}

		TEST_CASE("AudioClipData: validation rejects inconsistent clips" * doctest::skip(true))
		{
			const AudioClipData clip = MakePcmClip();
			CHECK(ValidateAudioClipData(clip).has_value());
			AudioClipData partial = clip;
			partial.Bytes.pop_back();
			CHECK(GetErrorCode(ValidateAudioClipData(partial)) == ErrorCode::Validation);
			AudioClipData streamedPcm = clip;
			streamedPcm.Stream = true;
			CHECK(GetErrorCode(ValidateAudioClipData(streamedPcm)) == ErrorCode::Validation);
			AudioClipData threeChannels = MakeEncodedClip();
			threeChannels.ChannelCount = 3;
			CHECK(GetErrorCode(ValidateAudioClipData(threeChannels)) == ErrorCode::Validation);
			AudioClipData slow = MakeEncodedClip();
			slow.SampleRate = 4000;
			CHECK(GetErrorCode(ValidateAudioClipData(slow)) == ErrorCode::Validation);
			AudioClipData empty = MakeEncodedClip();
			empty.FrameCount = 0;
			CHECK(GetErrorCode(ValidateAudioClipData(empty)) == ErrorCode::Validation);
			AudioClipData noBytes = MakeEncodedClip();
			noBytes.Bytes.clear();
			CHECK(GetErrorCode(ValidateAudioClipData(noBytes)) == ErrorCode::Validation);
			AudioClipData unknown = MakeEncodedClip();
			unknown.Encoding = static_cast<AudioClipEncoding>(42);
			CHECK(GetErrorCode(ValidateAudioClipData(unknown)) == ErrorCode::Validation);
		}

		TEST_CASE("AudioClipData: truncation, trailing bytes and bad header fields fail with Parse" * doctest::skip(true))
		{
			const Buffer payload = SerializeAudioClipPayload(MakePcmClip());
			Buffer truncated = payload;
			truncated.pop_back();
			CHECK(GetErrorCode(DeserializeAudioClipPayload(truncated)) == ErrorCode::Parse);
			Buffer trailing = payload;
			trailing.push_back(std::byte{ 0 });
			CHECK(GetErrorCode(DeserializeAudioClipPayload(trailing)) == ErrorCode::Parse);
			Buffer badStream = payload;
			badStream[1] = std::byte{ 2 };
			CHECK(GetErrorCode(DeserializeAudioClipPayload(badStream)) == ErrorCode::Parse);
			Buffer reserved = payload;
			reserved[2] = std::byte{ 1 };
			CHECK(GetErrorCode(DeserializeAudioClipPayload(reserved)) == ErrorCode::Parse);
			Buffer encoding = payload;
			encoding[0] = std::byte{ 99 };
			CHECK(GetErrorCode(DeserializeAudioClipPayload(encoding)) == ErrorCode::Parse);
		}

		TEST_CASE("AudioClipData: 10,000 seeded mutations of a payload never crash" * doctest::skip(true))
		{
			const Buffer payload = SerializeAudioClipPayload(MakePcmClip());
			Random random(0xA0D1);
			for (int iteration = 0; iteration < 10000; ++iteration)
			{
				Buffer mutated = payload;
				const size_t index = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()) - 1));
				mutated[index] = static_cast<std::byte>(random.NextU32() & 0xFF);
				if (random.NextBool(0.2))
					mutated.resize(static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()))));
				const Result<AudioClipData> read = DeserializeAudioClipPayload(mutated);
				if (read.has_value())
					CHECK(ValidateAudioClipData(*read).has_value());
			}
		}

		TEST_CASE("AudioClipData: a cooked clip loads through the built-in loaders" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			const AudioClipData clip = MakeEncodedClip();
			const Buffer cooked = CookAudioClip(clip, 3);
			const Result<CookedArtifactView> view = ReadCookedArtifact(cooked, AssetType::AudioClip, AudioClipData::FormatVersion);
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			CHECK(view->Header.ImporterVersion == 3);
			const Result<AssetRef<Asset>> loaded = loaders.Load(cooked, { .Registry = registry.get(), .Handle = AssetHandle(0x1234) });
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			const AssetRef<AudioClipData> typed = AssetCast<AudioClipData>(*loaded);
			REQUIRE(typed != nullptr);
			CheckEqual(*typed, clip);
			const Result<AssetRef<AudioClipData>> direct = LoadCookedAudioClip(cooked);
			REQUIRE(direct.has_value());
			CheckEqual(**direct, clip);
		}
	}

}
