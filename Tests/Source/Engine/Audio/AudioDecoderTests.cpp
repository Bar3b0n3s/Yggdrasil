#include "TestsPCH.h"

#include "Engine/Audio/AudioDecoder.h"

#include "Engine/Core/FileSystem.h"
#include "Support/AudioTestData.h"
#include "Support/TestData.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Probing and decoding encoded audio (Architecture §7.4 AudioImporter, §10.1 clips that decode at registration). Skipped
// skeletons of the M12 contract (Docs/Decisions/0015-m12-decisions.md): stream A implements the decoder and its fixtures
// (Tests/Data/Assets/Audio: Tone.wav and Tone.flac from its committed generator, Tone.mp3 and Tone.ogg pinned with their
// licences in Tests/Data/LICENSES.md) and removes the skips.

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

		// The bytes of Tests/Data/<relative>; fails the test case on error.
		Buffer ReadFixture(std::string_view relative)
		{
			Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath(relative));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			return std::move(*bytes);
		}

	}

	TEST_SUITE("Audio")
	{
		TEST_CASE("AudioDecoder: the formats name themselves")
		{
			CHECK(EncodedAudioFormatToString(EncodedAudioFormat::Wav) == "Wav");
			CHECK(EncodedAudioFormatToString(EncodedAudioFormat::Flac) == "Flac");
			CHECK(EncodedAudioFormatToString(EncodedAudioFormat::Mp3) == "Mp3");
			CHECK(EncodedAudioFormatToString(EncodedAudioFormat::Vorbis) == "Vorbis");
			const EncodedAudioInfo info{ .Format = EncodedAudioFormat::Wav, .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 24000 };
			CHECK(info.GetDurationSeconds() == doctest::Approx(0.5));
			CHECK(EncodedAudioInfo{}.GetDurationSeconds() == 0.0);
		}

		TEST_CASE("AudioDecoder: a WAV file is probed with its exact frame count" * doctest::skip(true))
		{
			const Test::TestToneSpecification tone{ .SampleRate = 44100, .ChannelCount = 2, .FrameCount = 12345, .Frequency = 440.0, .Amplitude = 0.5 };
			const Result<EncodedAudioInfo> info = ProbeEncodedAudio(Test::MakeToneWav(tone));
			REQUIRE_MESSAGE(info.has_value(), info.error().ToString());
			CHECK(info->Format == EncodedAudioFormat::Wav);
			CHECK(info->SampleRate == 44100);
			CHECK(info->ChannelCount == 2);
			CHECK(info->FrameCount == 12345);
		}

		TEST_CASE("AudioDecoder: decoding a WAV file returns its samples" * doctest::skip(true))
		{
			const Test::TestToneSpecification tone{ .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 480, .Frequency = 1000.0, .Amplitude = 0.5 };
			const Result<DecodedAudio> decoded = DecodeEncodedAudio(Test::MakeToneWav(tone));
			REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
			const std::vector<int16_t> expected = Test::MakeTonePcm16(tone);
			REQUIRE(decoded->Samples.size() == expected.size());
			for (size_t index = 0; index < expected.size(); ++index)
				CHECK(decoded->Samples[index] == doctest::Approx(static_cast<float>(expected[index]) / 32768.0f).epsilon(0.0001));
		}

		TEST_CASE("AudioDecoder: WAV, FLAC, MP3 and Ogg Vorbis fixtures decode" * doctest::skip(true))
		{
			struct Fixture
			{
				std::string_view Path{};
				EncodedAudioFormat Format = EncodedAudioFormat::Wav;
			};
			const Fixture fixtures[] = {
				{ "Assets/Audio/Tone.wav", EncodedAudioFormat::Wav },
				{ "Assets/Audio/Tone.flac", EncodedAudioFormat::Flac },
				{ "Assets/Audio/Tone.mp3", EncodedAudioFormat::Mp3 },
				{ "Assets/Audio/Tone.ogg", EncodedAudioFormat::Vorbis },
			};
			for (const Fixture& fixture : fixtures)
			{
				CAPTURE(std::string(fixture.Path));
				const Result<DecodedAudio> decoded = DecodeEncodedAudio(ReadFixture(fixture.Path));
				REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
				CHECK(decoded->Info.Format == fixture.Format);
				CHECK(decoded->Info.FrameCount > 0);
				CHECK((decoded->Info.ChannelCount == 1 || decoded->Info.ChannelCount == 2));
				CHECK(decoded->Samples.size() == decoded->Info.FrameCount * decoded->Info.ChannelCount);
				const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(ReadFixture(fixture.Path));
				REQUIRE(probed.has_value());
				CHECK(*probed == decoded->Info);
			}
		}

		TEST_CASE("AudioDecoder: bytes that are no audio file are a Parse error naming the formats" * doctest::skip(true))
		{
			const Buffer text = { std::byte{ 'h' }, std::byte{ 'e' }, std::byte{ 'l' }, std::byte{ 'l' }, std::byte{ 'o' } };
			const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(text);
			REQUIRE_FALSE(probed.has_value());
			CHECK(probed.error().GetCode() == ErrorCode::Parse);
			const std::string message = probed.error().ToString();
			CHECK(message.contains("WAV"));
			CHECK(message.contains("Ogg Vorbis"));
			CHECK(GetErrorCode(ProbeEncodedAudio({})) == ErrorCode::Parse);
		}

		TEST_CASE("AudioDecoder: a truncated file fails the probe" * doctest::skip(true))
		{
			const Test::TestToneSpecification tone{ .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 4800, .Frequency = 440.0, .Amplitude = 0.5 };
			Buffer wav = Test::MakeToneWav(tone);
			wav.resize(30);
			CHECK(GetErrorCode(ProbeEncodedAudio(wav)) == ErrorCode::Parse);
			Buffer ogg = ReadFixture("Assets/Audio/Tone.ogg");
			ogg.resize(ogg.size() / 2);
			CHECK(GetErrorCode(ProbeEncodedAudio(ogg)) == ErrorCode::Parse);
		}

		TEST_CASE("AudioDecoder: more than two channels, unsupported rates and empty files are Validation errors" * doctest::skip(true))
		{
			const Result<EncodedAudioInfo> surround = ProbeEncodedAudio(
				Test::MakeToneWav({ .SampleRate = 48000, .ChannelCount = 6, .FrameCount = 480, .Frequency = 440.0, .Amplitude = 0.5 }));
			REQUIRE_FALSE(surround.has_value());
			CHECK(surround.error().GetCode() == ErrorCode::Validation);
			CHECK(surround.error().ToString().contains("mono or stereo"));
			CHECK(GetErrorCode(ProbeEncodedAudio(Test::MakeToneWav({ .SampleRate = 4000, .ChannelCount = 1, .FrameCount = 480 })))
				== ErrorCode::Validation);
			CHECK(GetErrorCode(ProbeEncodedAudio(Test::MakeToneWav({ .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 0 })))
				== ErrorCode::Validation);
		}
	}

}
