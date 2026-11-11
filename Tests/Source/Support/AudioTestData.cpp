#include "TestsPCH.h"
#include "Support/AudioTestData.h"

#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/DetMath.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	namespace Test {

		// Round half away from zero, exact for the magnitudes of 16-bit samples.
		static int16_t QuantizeSample(double value)
		{
			const double scaled = value * 32767.0;
			const double rounded = scaled >= 0.0 ? std::floor(scaled + 0.5) : -std::floor(-scaled + 0.5);
			return static_cast<int16_t>(rounded);
		}

		std::vector<int16_t> MakeTonePcm16(const TestToneSpecification& tone)
		{
			std::vector<int16_t> samples;
			samples.reserve(static_cast<size_t>(tone.FrameCount) * tone.ChannelCount);
			const double step = 2.0 * std::numbers::pi * tone.Frequency / static_cast<double>(tone.SampleRate);
			for (uint64_t frame = 0; frame < tone.FrameCount; ++frame)
			{
				const int16_t sample = QuantizeSample(tone.Amplitude * DetMath::Sin(step * static_cast<double>(frame)));
				for (uint32_t channel = 0; channel < tone.ChannelCount; ++channel)
					samples.push_back(sample);
			}
			return samples;
		}

		Buffer MakeTonePcm16Bytes(const TestToneSpecification& tone)
		{
			const std::vector<int16_t> samples = MakeTonePcm16(tone);
			BinaryWriter writer;
			for (const int16_t sample : samples)
				writer.WriteI16(sample);
			return writer.TakeBuffer();
		}

		// A RIFF/WAVE file with a 16-byte fmt chunk of `format` and `bitsPerSample`, then `data`.
		static Buffer MakeWav(uint16_t format, uint32_t sampleRate, uint32_t channelCount, uint16_t bitsPerSample, std::span<const std::byte> data)
		{
			const auto dataSize = static_cast<uint32_t>(data.size());
			const uint32_t blockAlign = channelCount * (bitsPerSample / 8u);
			BinaryWriter writer;
			writer.WriteBytes(AsBytes("RIFF"));
			writer.WriteU32(36 + dataSize);
			writer.WriteBytes(AsBytes("WAVE"));
			writer.WriteBytes(AsBytes("fmt "));
			writer.WriteU32(16);
			writer.WriteU16(format);
			writer.WriteU16(static_cast<uint16_t>(channelCount));
			writer.WriteU32(sampleRate);
			writer.WriteU32(sampleRate * blockAlign);
			writer.WriteU16(static_cast<uint16_t>(blockAlign));
			writer.WriteU16(bitsPerSample);
			writer.WriteBytes(AsBytes("data"));
			writer.WriteU32(dataSize);
			writer.WriteBytes(data);
			return writer.TakeBuffer();
		}

		Buffer MakeToneWav(const TestToneSpecification& tone)
		{
			const Buffer data = MakeTonePcm16Bytes(tone);
			return MakeWav(1, tone.SampleRate, tone.ChannelCount, 16, data); // PCM
		}

		Buffer MakeFloatWav(std::span<const float> samples, uint32_t sampleRate, uint32_t channelCount)
		{
			BinaryWriter data;
			for (const float sample : samples)
				data.WriteF32(sample);
			const Buffer bytes = data.TakeBuffer();
			return MakeWav(3, sampleRate, channelCount, 32, bytes); // IEEE float
		}

		uint32_t OggCrc(std::span<const std::byte> page)
		{
			uint32_t crc = 0;
			for (const std::byte value : page)
			{
				crc ^= std::to_integer<uint32_t>(value) << 24;
				for (int bit = 0; bit < 8; ++bit)
					crc = (crc & 0x80000000u) != 0 ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
			}
			return crc;
		}

		std::vector<OggTestPage> SplitOggPages(std::span<const std::byte> bytes)
		{
			const auto read = [bytes](size_t offset, size_t count)
			{
				uint64_t value = 0;
				for (size_t index = 0; index < count; ++index)
					value |= std::to_integer<uint64_t>(bytes[offset + index]) << (8 * index);
				return value;
			};
			std::vector<OggTestPage> pages;
			size_t offset = 0;
			while (offset < bytes.size())
			{
				REQUIRE(bytes.size() - offset >= 27);
				REQUIRE(std::memcmp(bytes.data() + offset, "OggS", 4) == 0);
				OggTestPage page;
				page.Flags = std::to_integer<uint8_t>(bytes[offset + 5]);
				page.Granule = read(offset + 6, 8);
				page.Serial = static_cast<uint32_t>(read(offset + 14, 4));
				page.Sequence = static_cast<uint32_t>(read(offset + 18, 4));
				const size_t segments = std::to_integer<size_t>(bytes[offset + 26]);
				REQUIRE(bytes.size() - offset >= 27 + segments);
				size_t bodySize = 0;
				for (size_t segment = 0; segment < segments; ++segment)
				{
					page.Lacing.push_back(std::to_integer<uint8_t>(bytes[offset + 27 + segment]));
					bodySize += page.Lacing.back();
				}
				const size_t body = offset + 27 + segments;
				REQUIRE(bytes.size() - body >= bodySize);
				page.Body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(body), bytes.begin() + static_cast<std::ptrdiff_t>(body + bodySize));
				pages.push_back(std::move(page));
				offset = body + bodySize;
			}
			return pages;
		}

		Buffer JoinOggPages(std::span<const OggTestPage> pages)
		{
			Buffer bytes;
			const auto write = [&bytes](uint64_t value, size_t count)
			{
				for (size_t index = 0; index < count; ++index)
					bytes.push_back(static_cast<std::byte>(value >> (8 * index)));
			};
			for (const OggTestPage& page : pages)
			{
				const size_t start = bytes.size();
				for (const char character : std::string_view("OggS"))
					bytes.push_back(static_cast<std::byte>(character));
				write(0, 1); // version
				write(page.Flags, 1);
				write(page.Granule, 8);
				write(page.Serial, 4);
				write(page.Sequence, 4);
				write(0, 4); // the CRC, written below
				write(page.Lacing.size(), 1);
				for (const uint8_t lacing : page.Lacing)
					write(lacing, 1);
				bytes.insert(bytes.end(), page.Body.begin(), page.Body.end());
				const uint32_t crc = OggCrc(std::span<const std::byte>(bytes).subspan(start));
				for (size_t index = 0; index < 4; ++index)
					bytes[start + 22 + index] = static_cast<std::byte>(crc >> (8 * index));
			}
			return bytes;
		}

	}

}
