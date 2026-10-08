#include "TestsPCH.h"

#include "Support/AudioTestData.h"

#include "Engine/Core/BinaryReader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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
	}

}
