#include "TestsPCH.h"

#include "Engine/Audio/AudioDecoder.h"

#include "Engine/Core/FileSystem.h"
#include "Support/AudioTestData.h"
#include "Support/TestData.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Probing and decoding encoded audio (Architecture §7.4 AudioImporter, §10.1 clips that decode at registration;
// Docs/Decisions/0015-m12-decisions.md). The format fixtures are Tests/Data/Assets/Audio: Tone.wav and Tone.flac from their
// committed generator, Tone.mp3 and Tone.ogg pinned with their licences in Tests/Data/LICENSES.md.

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

		TEST_CASE("AudioDecoder: a WAV file is probed with its exact frame count")
		{
			const Test::TestToneSpecification tone{ .SampleRate = 44100, .ChannelCount = 2, .FrameCount = 12345, .Frequency = 440.0, .Amplitude = 0.5 };
			const Result<EncodedAudioInfo> info = ProbeEncodedAudio(Test::MakeToneWav(tone));
			REQUIRE_MESSAGE(info.has_value(), info.error().ToString());
			CHECK(info->Format == EncodedAudioFormat::Wav);
			CHECK(info->SampleRate == 44100);
			CHECK(info->ChannelCount == 2);
			CHECK(info->FrameCount == 12345);
		}

		TEST_CASE("AudioDecoder: decoding a WAV file returns its samples")
		{
			const Test::TestToneSpecification tone{ .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 480, .Frequency = 1000.0, .Amplitude = 0.5 };
			const Result<DecodedAudio> decoded = DecodeEncodedAudio(Test::MakeToneWav(tone));
			REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
			const std::vector<int16_t> expected = Test::MakeTonePcm16(tone);
			REQUIRE(decoded->Samples.size() == expected.size());
			for (size_t index = 0; index < expected.size(); ++index)
				CHECK(decoded->Samples[index] == doctest::Approx(static_cast<float>(expected[index]) / 32768.0f).epsilon(0.0001));
		}

		TEST_CASE("AudioDecoder: WAV, FLAC, MP3 and Ogg Vorbis fixtures decode")
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

		TEST_CASE("AudioDecoder: bytes that are no audio file are a Parse error naming the formats")
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

		TEST_CASE("AudioDecoder: an Ogg file of another codec is a Parse error with a conversion hint")
		{
			// The first page of an Ogg Opus stream: the 27-byte page header, one lacing value and the "OpusHead" packet.
			Buffer opus;
			const auto append = [&opus](std::string_view text)
			{
				for (const char character : text)
					opus.push_back(static_cast<std::byte>(character));
			};
			append("OggS");
			opus.insert(opus.end(), { std::byte{ 0 }, std::byte{ 2 } });  // version, beginning of stream
			opus.insert(opus.end(), 20, std::byte{ 0 });                  // granule position, serial, sequence, CRC
			opus.insert(opus.end(), { std::byte{ 1 }, std::byte{ 19 } }); // one segment of 19 bytes
			append("OpusHead");
			opus.insert(opus.end(), { std::byte{ 1 }, std::byte{ 2 } }); // version, channels
			opus.insert(opus.end(), 9, std::byte{ 0 });
			const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(opus);
			REQUIRE_FALSE(probed.has_value());
			CHECK(probed.error().GetCode() == ErrorCode::Parse);
			CHECK(probed.error().GetMessageText().contains("Opus"));
			CHECK(probed.error().GetHint().contains("Ogg Vorbis"));
		}

		TEST_CASE("AudioDecoder: a truncated file fails the probe")
		{
			const Test::TestToneSpecification tone{ .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 4800, .Frequency = 440.0, .Amplitude = 0.5 };
			Buffer wav = Test::MakeToneWav(tone);
			wav.resize(30);
			CHECK(GetErrorCode(ProbeEncodedAudio(wav)) == ErrorCode::Parse);
			Buffer ogg = ReadFixture("Assets/Audio/Tone.ogg");
			ogg.resize(ogg.size() / 2);
			CHECK(GetErrorCode(ProbeEncodedAudio(ogg)) == ErrorCode::Parse);
		}

		TEST_CASE("AudioDecoder: a float WAV file with a non-finite sample is a Validation error naming the frame")
		{
			// An IEEE-float WAV can hold NaN and infinities, which the mixer would carry to the device and the capture buffer.
			// The probe decodes every frame, so the importer refuses such a file whether the clip will stream or not.
			std::vector<float> samples(4800 * 2, 0.25f);
			const Buffer finite = Test::MakeFloatWav(samples, 48000, 2);
			const Result<DecodedAudio> decoded = DecodeEncodedAudio(finite);
			REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
			CHECK(decoded->Samples == samples);

			const float infinity = std::numeric_limits<float>::infinity();
			for (const float bad : { std::numeric_limits<float>::quiet_NaN(), infinity, -infinity })
			{
				CAPTURE(bad);
				std::vector<float> withBad = samples;
				withBad[4500 * 2 + 1] = bad; // frame 4500, in the second decode chunk
				const Buffer wav = Test::MakeFloatWav(withBad, 48000, 2);
				const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(wav);
				REQUIRE_FALSE(probed.has_value());
				CHECK(probed.error().GetCode() == ErrorCode::Validation);
				CHECK(probed.error().GetMessageText().contains("non-finite"));
				CHECK(probed.error().GetMessageText().contains("frame 4500"));
				CHECK_FALSE(probed.error().GetHint().empty());
				CHECK(GetErrorCode(DecodeEncodedAudio(wav)) == ErrorCode::Validation);
			}
		}

		TEST_CASE("AudioDecoder: more than two channels, unsupported rates and empty files are Validation errors")
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
