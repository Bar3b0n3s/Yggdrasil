#include "TestsPCH.h"

#include "Support/AudioTestData.h"

#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/FileSystem.h"
#include "Support/TestData.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("AudioTestData: a tone is a quantized sine on every channel")
		{
			const Test::TestToneSpecification tone{ .SampleRate = 48000, .ChannelCount = 2, .FrameCount = 480, .Frequency = 1000.0, .Amplitude = 0.5 };
			const std::vector<int16_t> samples = Test::MakeTonePcm16(tone);
			REQUIRE(samples.size() == 960);
			CHECK(samples[0] == 0);
			CHECK(samples[1] == 0);
			// A quarter period of 1 kHz at 48 kHz is 12 frames: the peak, 0.5 of full scale (16383.5 before rounding).
			CHECK(samples[24] >= 16383);
			CHECK(samples[24] <= 16384);
			for (size_t frame = 0; frame < 480; ++frame)
				CHECK(samples[frame * 2] == samples[frame * 2 + 1]);
			CHECK(Test::MakeTonePcm16(tone) == samples);
			CHECK(Test::MakeTonePcm16Bytes(tone).size() == samples.size() * sizeof(int16_t));
		}

		TEST_CASE("AudioTestData: a WAV file has the canonical PCM header followed by the tone")
		{
			const Test::TestToneSpecification tone{ .SampleRate = 44100, .ChannelCount = 1, .FrameCount = 100, .Frequency = 440.0, .Amplitude = 0.25 };
			const Buffer wav = Test::MakeToneWav(tone);
			const Buffer pcm = Test::MakeTonePcm16Bytes(tone);
			REQUIRE(wav.size() == 44 + pcm.size());
			BinaryReader reader(wav);
			// Empty past the end, so the CHECK that compares it fails.
			const auto readTag = [&reader]()
			{
				const Result<std::span<const std::byte>> bytes = reader.ReadBytes(4);
				return bytes.has_value() ? std::string(AsStringView(*bytes)) : std::string();
			};
			CHECK(readTag() == "RIFF");
			CHECK(reader.ReadU32().value_or(0) == 36 + pcm.size());
			CHECK(readTag() == "WAVE");
			CHECK(readTag() == "fmt ");
			CHECK(reader.ReadU32().value_or(0) == 16);
			CHECK(reader.ReadU16().value_or(0) == 1);
			CHECK(reader.ReadU16().value_or(0) == 1);
			CHECK(reader.ReadU32().value_or(0) == 44100);
			CHECK(reader.ReadU32().value_or(0) == 88200);
			CHECK(reader.ReadU16().value_or(0) == 2);
			CHECK(reader.ReadU16().value_or(0) == 16);
			CHECK(readTag() == "data");
			CHECK(reader.ReadU32().value_or(0) == pcm.size());
			CHECK(std::equal(pcm.begin(), pcm.end(), wav.begin() + 44));
		}

		TEST_CASE("AudioTestData: a float WAV file has the IEEE-float header followed by the samples as given")
		{
			const float infinity = std::numeric_limits<float>::infinity();
			const std::vector<float> samples = { 0.25f, -0.5f, infinity, 1.0e30f };
			const Buffer wav = Test::MakeFloatWav(samples, 22050, 2);
			REQUIRE(wav.size() == 44 + samples.size() * sizeof(float));
			BinaryReader reader(wav);
			const auto readTag = [&reader]()
			{
				const Result<std::span<const std::byte>> bytes = reader.ReadBytes(4);
				return bytes.has_value() ? std::string(AsStringView(*bytes)) : std::string();
			};
			CHECK(readTag() == "RIFF");
			CHECK(reader.ReadU32().value_or(0) == 36 + samples.size() * sizeof(float));
			CHECK(readTag() == "WAVE");
			CHECK(readTag() == "fmt ");
			CHECK(reader.ReadU32().value_or(0) == 16);
			CHECK(reader.ReadU16().value_or(0) == 3);
			CHECK(reader.ReadU16().value_or(0) == 2);
			CHECK(reader.ReadU32().value_or(0) == 22050);
			CHECK(reader.ReadU32().value_or(0) == 22050 * 8);
			CHECK(reader.ReadU16().value_or(0) == 8);
			CHECK(reader.ReadU16().value_or(0) == 32);
			CHECK(readTag() == "data");
			CHECK(reader.ReadU32().value_or(0) == samples.size() * sizeof(float));
			for (const float expected : samples)
				CHECK(reader.ReadF32().value_or(0.0f) == expected);
		}

		// M23's milestone review: the committed Ogg fixtures split into pages that join back to their bytes, CRCs included, so
		// a variant built from them changes only what its test changes.
		TEST_CASE("AudioTestData: an Ogg file splits into pages that join back to its bytes")
		{
			for (const std::string_view fixture : { "Assets/Audio/Tone.ogg", "Assets/Audio/Water.opus" })
			{
				CAPTURE(std::string(fixture));
				Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath(fixture));
				REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
				const std::vector<Test::OggTestPage> pages = Test::SplitOggPages(*bytes);
				REQUIRE(pages.size() > 2);
				CHECK((pages.front().Flags & Test::OggBeginsStream) != 0);
				CHECK((pages.back().Flags & Test::OggEndsStream) != 0);
				CHECK(Test::JoinOggPages(pages) == *bytes);
				// The first page's CRC, over the page with its CRC field zero, is the one stored at bytes 22 to 25.
				const size_t firstSize = 27 + pages.front().Lacing.size() + pages.front().Body.size();
				Buffer first(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(firstSize));
				uint32_t stored = 0;
				for (size_t index = 0; index < 4; ++index)
				{
					stored |= std::to_integer<uint32_t>(first[22 + index]) << (8 * index);
					first[22 + index] = std::byte{ 0 };
				}
				CHECK(Test::OggCrc(first) == stored);
			}
		}
	}

}
