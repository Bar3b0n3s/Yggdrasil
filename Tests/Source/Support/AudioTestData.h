#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"

#include <cstdint>
#include <vector>

// Deterministic audio built in memory for the audio tests of M12 (Docs/Decisions/0015-m12-decisions.md): the clips the
// AudioEngine, AudioDecoder, AudioImporter and AudioSystem tests play, decode and import, so no test depends on a fixture
// file it does not need. Samples are computed with Core/DetMath, so the bytes are identical on every platform and in every
// configuration. Implemented by the M12 contract.

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

	}

}
