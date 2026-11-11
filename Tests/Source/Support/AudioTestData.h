#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Deterministic audio built in memory for the audio tests of M12 (Docs/Decisions/0015-m12-decisions.md): the clips the
// AudioEngine, AudioDecoder, AudioImporter and AudioSystem tests play, decode and import, so no test depends on a fixture
// file it does not need. Samples are computed with Core/DetMath, so the bytes are identical on every platform and in every
// configuration. Implemented by the M12 contract. Since M23's milestone review (Docs/Decisions/0034-m23-contract.md) also
// the Ogg pages (RFC 3533) the AudioDecoder and AudioEngine tests split a committed Ogg file into and join again, with
// their CRCs, to build its variants (truncated, chained, multiplexed or mutated files).

namespace Engine {

	namespace Test {

		// What a test tone is made of.
		struct TestToneSpecification
		{
			uint32_t SampleRate = 48000;
			uint32_t ChannelCount = 1; // every channel carries the same tone
			uint64_t FrameCount = 4800;
			double Frequency = 440.0; // Hz
			double Amplitude = 0.5;   // of full scale, 0 to 1
		};

		// The tone's interleaved little-endian signed 16-bit samples: sample n of each channel is
		// round(Amplitude * 32767 * DetMath::Sin(2 pi Frequency n / SampleRate)), half away from zero.
		[[nodiscard]] std::vector<int16_t> MakeTonePcm16(const TestToneSpecification& tone);

		// The samples of MakeTonePcm16 as bytes (an AudioClipFormat::Pcm16 clip's Bytes).
		[[nodiscard]] Buffer MakeTonePcm16Bytes(const TestToneSpecification& tone);

		// A canonical 44-byte-header RIFF/WAVE file (format 1, PCM, 16 bits) of MakeTonePcm16's samples.
		[[nodiscard]] Buffer MakeToneWav(const TestToneSpecification& tone);

		// A 44-byte-header RIFF/WAVE file (format 3, IEEE float, 32 bits) of the interleaved `samples`, written as given:
		// NaN and infinities included, which is how the decoder tests build files that hold non-finite samples. A partial
		// trailing frame is written too.
		[[nodiscard]] Buffer MakeFloatWav(std::span<const float> samples, uint32_t sampleRate, uint32_t channelCount);

		// The header flags of an Ogg page that begin and end a logical stream.
		inline constexpr uint8_t OggBeginsStream = 0x02;
		inline constexpr uint8_t OggEndsStream = 0x04;

		// One page of an Ogg file, the fields a test changes.
		struct OggTestPage
		{
			uint8_t Flags = 0;
			uint64_t Granule = 0;
			uint32_t Serial = 0;
			uint32_t Sequence = 0;
			std::vector<uint8_t> Lacing{};
			Buffer Body{};
		};

		// Ogg's CRC-32 (RFC 3533): polynomial 0x04C11DB7, no reflection, initial value and final XOR 0, over a page whose
		// CRC field is zero.
		[[nodiscard]] uint32_t OggCrc(std::span<const std::byte> page);

		// The pages of a well-formed Ogg file; fails the test case otherwise.
		[[nodiscard]] std::vector<OggTestPage> SplitOggPages(std::span<const std::byte> bytes);

		// The bytes of `pages`, each with its CRC.
		[[nodiscard]] Buffer JoinOggPages(std::span<const OggTestPage> pages);

	}

}
