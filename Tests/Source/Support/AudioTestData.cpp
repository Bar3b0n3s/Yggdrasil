#include "TestsPCH.h"
#include "Support/AudioTestData.h"

#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/DetMath.h"

#include <cmath>
#include <numbers>

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

	}

}
